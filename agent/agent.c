/*
 * agent.c — orchestration between user input, LLM turns, and tool execution.
 *
 * Part 3: the flat MessageList history has been replaced by a Context pointer
 * that manages token budgets through offload and summary policies.
 * Session persistence provides crash recovery through an append‑only log.
 */
#include "agent.h"

#include "config.h"
#include "context/context.h"
#include "context/session.h"
#include "llm_client.h"
#include "message.h"
#include "tools/executor.h"
#include "tools/tools.h"
#include "ui/ui.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TURNS 20

static const char AGENT_SYSTEM_TEMPLATE[] =
    "You are a coding agent running in the CLI at %s.\n"
    "Use the provided tools when you need to run shell commands.\n"
    "Return a short, final text reply when the task is done.";

struct Agent {
  char *system_prompt;
  char *last_reply;
  Context *ctx;
  /* Evaluation metrics — reset per agent_chat call. */
  int turn_count;
  int prompt_tokens;
  int completion_tokens;
};

/* ── Replay callback ────────────────────────────────────────────────── */

static void replay_push(void *user_data, const char *msg) {
  ctx_push((Context *)user_data, strdup(msg));
}

/* ── Public API ──────────────────────────────────────────────────────── */

Agent *agent_create(void) {
  Agent *a = calloc(1, sizeof(*a));
  if (!a)
    return NULL;
  a->system_prompt = xasprintf(AGENT_SYSTEM_TEMPLATE, g_config.workdir);
  a->ctx = ctx_create(g_config.context_window);
  if (!a->ctx) {
    free(a->system_prompt);
    free(a);
    return NULL;
  }
  ctx_add_policy(a->ctx, &offload_policy);
  ctx_add_policy(a->ctx, &summary_policy);

  /* Replay auto session for crash recovery — session not yet attached so
     ctx_push does NOT append replayed messages back to the same file. */
  session_replay(g_config.workdir, "auto", replay_push, a->ctx);

  /* Now open the auto session for append-only logging of new messages. */
  Session *s = session_open(g_config.workdir, "auto");
  if (s)
    ctx_set_session(a->ctx, s);

  return a;
}

void agent_free(Agent *a) {
  if (!a)
    return;
  ctx_free(a->ctx);
  free(a->system_prompt);
  free(a->last_reply);
  free(a);
}

const char *agent_chat(Agent *a, const char *user_input) {
  if (!a || !user_input)
    return NULL;

  a->turn_count = 0;
  a->prompt_tokens = 0;
  a->completion_tokens = 0;

  char *user_json = msg_user_json(user_input);
  if (!user_json)
    return NULL;
  ctx_push(a->ctx, user_json);

  for (int turn = 0; turn < MAX_TURNS; turn++) {
    a->turn_count++;
    ui_begin_thinking();

    /* Reclaim context budget before building the next request. */
    char reclaim_err[256];
    if (ctx_reclaim(a->ctx, reclaim_err, sizeof(reclaim_err)) != 0) {
      fprintf(stderr, "context reclaim error: %s\n", reclaim_err);
      return NULL;
    }

    LLMResponse response = {0};
    if (llm_chat(ctx_history(a->ctx), a->system_prompt, g_config.model,
                 &response, NULL, 0) < 0) {
      fprintf(stderr, "llm_chat error\n");
      llm_response_free(&response);
      return NULL;
    }

    a->prompt_tokens += response.prompt_tokens;
    a->completion_tokens += response.completion_tokens;

    if (response.raw_message) {
      ctx_push(a->ctx, response.raw_message);
      response.raw_message = NULL;
    }

    if (response.n_tool_calls == 0) {
      free(a->last_reply);
      a->last_reply = xstrdup(response.content ? response.content : "");
      llm_response_free(&response);
      fprintf(stderr, "[metrics] turns=%d prompt=%d completion=%d total=%d\n",
              a->turn_count, a->prompt_tokens, a->completion_tokens,
              a->prompt_tokens + a->completion_tokens);
      return a->last_reply;
    }

    int call_count = response.n_tool_calls;
    char *out_msgs[call_count];
    char err[256];
    if (executor_run_tools(response.tool_calls, call_count, out_msgs,
                           err, sizeof(err)) < 0) {
      fprintf(stderr, "executor error: %s\n", err);
      llm_response_free(&response);
      return NULL;
    }
    for (int i = 0; i < call_count; i++)
      ctx_push(a->ctx, out_msgs[i]);
    llm_response_free(&response);
  }

  fprintf(stderr, "agent_chat: exceeded turn limit\n");
  return NULL;
}

/* ── Session commands ────────────────────────────────────────────────── */

int agent_handle_session(Agent *a, int argc, char **argv) {
  if (!a || argc < 1)
    return -1;

  const char *cmd = argv[0];

  if (strcmp(cmd, "list") == 0) {
    int count;
    char **list = session_list(g_config.workdir, &count);
    if (!list || count == 0) {
      printf("No saved sessions.\n");
    } else {
      printf("Sessions:\n");
      for (int i = 0; i < count; i++)
        printf("  %s\n", list[i]);
    }
    session_free_list(list, count);
    return 0;
  }

  if (strcmp(cmd, "save") == 0 && argc >= 2) {
    const char *name = argv[1];
    /* Copy auto.log → named.log */
    char src[4096], dst[4096];
    snprintf(src, sizeof(src), "%s/.agent/session/auto.log", g_config.workdir);
    snprintf(dst, sizeof(dst), "%s/.agent/session/%s.log", g_config.workdir, name);
    FILE *in = fopen(src, "r");
    if (!in) {
      printf("No active session to save.\n");
      return 0;
    }
    FILE *out = fopen(dst, "w");
    if (!out) {
      fclose(in);
      printf("Cannot write %s\n", dst);
      return 0;
    }
    char buf[65536];
    size_t n;
    int lines = 0;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
      fwrite(buf, 1, n, out);
      for (size_t i = 0; i < n; i++)
        if (buf[i] == '\n')
          lines++;
    }
    fclose(in);
    fclose(out);
    printf("Saved session as '%s' (%d messages).\n", name, lines);
    return 0;
  }

  if (strcmp(cmd, "load") == 0 && argc >= 2) {
    const char *name = argv[1];
    ctx_set_session(a->ctx, NULL);
    ctx_clear(a->ctx);

    int replayed = session_replay(g_config.workdir, name, replay_push, a->ctx);
    if (replayed == 0) {
      printf("Session '%s' not found or empty.\n", name);
    } else {
      printf("Loaded session '%s' (%d messages).\n", name, replayed);
    }

    Session *s = session_open(g_config.workdir, "auto");
    if (s)
      ctx_set_session(a->ctx, s);
    return 0;
  }

  if (strcmp(cmd, "new") == 0) {
    ctx_set_session(a->ctx, NULL);
    session_delete(g_config.workdir, "auto");
    ctx_clear(a->ctx);

    Session *s = session_open(g_config.workdir, "auto");
    if (s)
      ctx_set_session(a->ctx, s);
    printf("Started a new session.\n");
    return 0;
  }

  printf("Usage:  /session <command> [args]\n"
         "  list       — list saved sessions\n"
         "  save <n>   — save a named snapshot\n"
         "  load <n>   — load a saved session\n"
         "  new        — discard current session\n");
  return -1;
}
