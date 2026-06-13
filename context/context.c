/* Conversation context: message history plus budget-driven reclamation. */
#include "agent/llm_client.h"
#include "config.h"
#include "context/internal.h"
#include "context/session.h"
#include "message.h"
#include "util.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ── Core context operations ─────────────────────────────────────────── */

Context *ctx_create(int context_window) {
  Context *ctx = calloc(1, sizeof(*ctx));
  if (!ctx)
    return NULL;
  msg_list_init(&ctx->history);
  ctx->context_window = context_window;
  return ctx;
}

void ctx_free(Context *ctx) {
  if (!ctx)
    return;
  session_close(ctx->session);
  msg_list_free(&ctx->history);
  free(ctx);
}

void ctx_add_policy(Context *ctx, ContextPolicy *policy) {
  if (ctx->policy_count >= MAX_POLICIES) {
    fprintf(stderr, "[context] warning: max policies reached, ignoring %s\n",
            policy->name);
    return;
  }
  ctx->policies[ctx->policy_count++] = policy;
}

void ctx_push(Context *ctx, char *msg_json) {
  msg_list_push(&ctx->history, msg_json);
  if (ctx->session)
    session_append(ctx->session, msg_json);
}

void ctx_set_session(Context *ctx, Session *s) {
  ctx->session = s;
}

void ctx_clear(Context *ctx) {
  msg_list_free(&ctx->history);
  msg_list_init(&ctx->history);
}

int ctx_total_tokens(const Context *ctx) {
  int total = 0;
  for (int i = 0; i < ctx->history.len; i++)
    total += ctx_estimate_tokens(ctx->history.items[i]);
  return total;
}

float ctx_budget_usage(const Context *ctx) {
  if (ctx->context_window <= 0)
    return 0.0f;
  return (float)ctx_total_tokens(ctx) / (float)ctx->context_window;
}

const MessageList *ctx_history(const Context *ctx) { return &ctx->history; }

void ctx_replace_msg(Context *ctx, int index, char *new_json) {
  if (index < 0 || index >= ctx->history.len)
    return;
  free(ctx->history.items[index]);
  ctx->history.items[index] = new_json;
}

/*
 * Replace history[from:to) with a single message. Frees the removed ones and
 * shifts the tail left. Caller surrenders ownership of new_json.
 */
void ctx_replace_range(Context *ctx, int from, int to, char *new_json) {
  if (from < 0 || to > ctx->history.len || from >= to)
    return;

  for (int i = from; i < to; i++)
    free(ctx->history.items[i]);

  ctx->history.items[from] = new_json;

  int range = to - from;
  int tail = ctx->history.len - to;
  if (tail > 0) {
    memmove(&ctx->history.items[from + 1], &ctx->history.items[to],
            (size_t)tail * sizeof(char *));
  }
  ctx->history.len -= range - 1;
}

int ctx_reclaim(Context *ctx, char *err, size_t err_cap) {
  for (int i = 0; i < ctx->policy_count; i++) {
    ContextPolicy *p = ctx->policies[i];
    if (!p->should_apply(ctx))
      continue;

    int before = ctx_total_tokens(ctx);
    fprintf(stderr, "[context] policy \"%s\" triggered at %.1f%% (%d tokens)\n", p->name,
           ctx_budget_usage(ctx) * 100.0f, before);

    if (p->apply(ctx, err, err_cap) != 0)
      return -1;

    int after = ctx_total_tokens(ctx);
    if (after < before)
      fprintf(stderr, "[context] policy \"%s\" complete: %d -> %d tokens\n", p->name,
             before, after);
  }

  int total = ctx_total_tokens(ctx);
  if (ctx->context_window > 0 && total > ctx->context_window) {
    snprintf(err, err_cap, "context window exceeded (%d > %d tokens)", total,
             ctx->context_window);
    return -1;
  }
  return 0;
}

/* ── Offload policy ──────────────────────────────────────────────────── */

#define MIN_OFFLOAD_LEN 200

static bool offload_should_apply(Context *ctx) {
    return ctx_budget_usage(ctx) > g_config.offload_threshold;
}

static int offload_apply(Context *ctx, char *err, size_t err_cap) {
    (void)err;
    (void)err_cap;

    char agent_dir[PATH_MAX];
    snprintf(agent_dir, sizeof(agent_dir), "%s/.agent", g_config.workdir);
    mkdir(g_config.workdir, 0755);
    mkdir(agent_dir, 0755);

    char offload_dir[PATH_MAX];
    snprintf(offload_dir, sizeof(offload_dir), "%s/.agent/offload",
             g_config.workdir);
    mkdir(offload_dir, 0755);

    int limit = ctx->history.len - KEEP_RECENT_MSGS;
    if (limit < 0)
        limit = 0;

    for (int i = 0; i < limit; i++) {
        cJSON *msg = cJSON_Parse(ctx->history.items[i]);
        if (!msg)
            continue;

        const char *role = json_str(msg, "role");
        const char *content = json_str(msg, "content");

        if (!role || strcmp(role, "tool") != 0 || !content ||
            (int)strlen(content) < MIN_OFFLOAD_LEN) {
            cJSON_Delete(msg);
            continue;
        }
        if (strstr(content, ".agent/offload/") != NULL) {
            cJSON_Delete(msg);
            continue;
        }

        char fpath[PATH_MAX];
        snprintf(fpath, sizeof(fpath), "%s/%d.txt", offload_dir,
                 ctx->next_offload_id);

        FILE *fp = fopen(fpath, "w");
        if (!fp) {
            cJSON_Delete(msg);
            continue;
        }
        fwrite(content, 1, strlen(content), fp);
        fclose(fp);

        char preview[512];
        snprintf(preview, sizeof(preview),
                 "%.4s... (read_file .agent/offload/%d.txt)",
                 content, ctx->next_offload_id);

        cJSON_DeleteItemFromObject(msg, "content");
        cJSON_AddStringToObject(msg, "content", preview);

        char *new_json = cJSON_PrintUnformatted(msg);
        cJSON_Delete(msg);

        ctx_replace_msg(ctx, i, new_json);
        ctx->next_offload_id++;
    }

    return 0;
}

/* ── Summary policy ──────────────────────────────────────────────────── */

static const char SUMMARY_SYSTEM_PROMPT[] =
    "Summarize the key decisions, actions, and outcomes from the following "
    "conversation. Focus on what was done and what was decided. Be concise.";

static bool summary_should_apply(Context *ctx) {
    return ctx_budget_usage(ctx) > g_config.summary_threshold;
}

static int summary_apply(Context *ctx, char *err, size_t err_cap) {
    int total = ctx->history.len;
    int keep = KEEP_RECENT_MSGS;
    int to = total - keep;

    if (to <= 0)
        return 0;

    size_t body_len = 0;
    for (int i = 0; i < to; i++)
        body_len += strlen(ctx_history(ctx)->items[i]) + 1;

    char *body = xmalloc(body_len + 1);
    body[0] = '\0';
    for (int i = 0; i < to; i++) {
        if (i > 0)
            strcat(body, "\n");
        strcat(body, ctx_history(ctx)->items[i]);
    }

    MessageList prefix;
    msg_list_init(&prefix);
    msg_list_push(&prefix, msg_user_json(body));
    free(body);

    LLMResponse response = {0};
    int rc = llm_chat(&prefix, SUMMARY_SYSTEM_PROMPT, g_config.model,
                       &response, err, err_cap);
    msg_list_free(&prefix);

    if (rc != 0 || !response.content || response.content[0] == '\0') {
        /* LLM failure should not crash the agent — skip summary this round. */
        fprintf(stderr, "[context] policy \"summary\" skipped (LLM returned empty or error)\n");
        llm_response_free(&response);
        return 0;
    }

    char summary_buf[4096];
    snprintf(summary_buf, sizeof(summary_buf),
             "[Summary of previous conversation]\n%s", response.content);
    char *summary_json = msg_user_json(summary_buf);
    llm_response_free(&response);

    if (!summary_json) {
        if (err && err_cap > 0)
            snprintf(err, err_cap, "summary policy: failed to create summary message");
        return -1;
    }

    ctx_replace_range(ctx, 0, to, summary_json);
    return 0;
}

/* ── Policy definitions ──────────────────────────────────────────────── */

ContextPolicy offload_policy = {
    .name = "offload",
    .should_apply = offload_should_apply,
    .apply = offload_apply,
};

ContextPolicy summary_policy = {
    .name = "summary",
    .should_apply = summary_should_apply,
    .apply = summary_apply,
};
