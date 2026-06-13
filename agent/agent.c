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
    "For complex multi-step tasks you can delegate to the subagent tool.\n"
    "Return a short, final text reply when the task is done.";

struct Agent {
  char *system_prompt;
  char *last_reply;
  Context *ctx;
  /* Evaluation metrics — reset per agent_chat call. */
  int turn_count;
  int prompt_tokens;
  int completion_tokens;
  char *last_metrics;  /* formatted metrics string, for main.c to print after ui_idle */
};

/* ── Public API ──────────────────────────────────────────────────────── */

Agent *agent_create(void) {
  Agent *a = calloc(1, sizeof(*a));
  if (!a)
    return NULL;
  a->last_metrics = NULL;
  a->system_prompt = xasprintf(AGENT_SYSTEM_TEMPLATE, g_config.workdir);
  a->ctx = ctx_create(g_config.context_window);
  if (!a->ctx) {
    free(a->system_prompt);
    free(a);
    return NULL;
  }
  ctx_add_policy(a->ctx, &offload_policy);
  ctx_add_policy(a->ctx, &summary_policy);


  return a;
}

void agent_free(Agent *a) {
  if (!a)
    return;
  ctx_free(a->ctx);
  free(a->system_prompt);
  free(a->last_reply);
  free(a->last_metrics);
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
      /* Strip common model artifacts from output. */
      const char *raw = response.content ? response.content : "";
      const char *tag = strstr(raw, "<tool_call>");
      if (tag) {
          size_t len = (size_t)(tag - raw);
          char *s = xmalloc(len + 1);
          memcpy(s, raw, len);
          s[len] = '\0';
          a->last_reply = s;
      } else {
          a->last_reply = xstrdup(raw);
      }
      llm_response_free(&response);
      free(a->last_metrics);
      a->last_metrics = xasprintf("\033[36m▸ Metrics\033[0m  turns=%d  prompt=%d  completion=%d  total=%d",
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

Context *agent_get_context(Agent *a) {
    return a ? a->ctx : NULL;
}

const char *agent_last_metrics(Agent *a) {
    return a ? a->last_metrics : NULL;
}
