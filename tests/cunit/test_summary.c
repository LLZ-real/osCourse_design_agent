#include "config.h"
#include "context/context.h"
#include "context/internal.h"
#include "fake_summarizer.h"
#include "message.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0, total = 0;

static void check(const char *name, int ok) {
  total++;
  if (ok) {
    passed++;
    printf("  [PASS] %s\n", name);
  } else {
    printf("  [FAIL] %s\n", name);
  }
}

static char *make_msg(const char *role, const char *content) {
  char *buf = malloc(strlen(role) + strlen(content) + 64);
  sprintf(buf, "{\"role\":\"%s\",\"content\":\"%s\"}", role, content);
  return buf;
}

static const char *role_at(const Context *ctx, int i) {
  static char buf[64];
  cJSON *m = cJSON_Parse(ctx_history(ctx)->items[i]);
  snprintf(buf, sizeof(buf), "%s", json_str(m, "role"));
  cJSON_Delete(m);
  return buf;
}

static const char *content_at(const Context *ctx, int i) {
  static char buf[2048];
  cJSON *m = cJSON_Parse(ctx_history(ctx)->items[i]);
  snprintf(buf, sizeof(buf), "%s",
           json_str(m, "content") ? json_str(m, "content") : "");
  cJSON_Delete(m);
  return buf;
}

int main(void) {
  g_config.context_window = 1000;
  g_config.summary_threshold = 0.1f; /* always fire */
  snprintf(g_config.model, sizeof(g_config.model), "test-model");

  printf("=== summary: collapses old range into one summary message ===\n");

  fake_summarizer_reset();
  fake_summarizer_set_response(
      "Goal: user asked X. Decisions: did Y. Progress: Z done.");

  Context *ctx = ctx_create(g_config.context_window);

  /* Build enough messages for the current KEEP_RECENT_MSGS value to leave
   * a non-empty droppable prefix. The policy should collapse that prefix
   * into one summary message. */
  char turn[64];
  for (int i = 0; i < 15; i++) {
    snprintf(turn, sizeof(turn), "turn-%d body", i);
    ctx_push(ctx, make_msg(i % 2 == 0 ? "user" : "assistant", turn));
  }

  int len_before = ctx_history(ctx)->len;
  int rc = summary_policy.apply(ctx, NULL, 0);

  check("apply returned 0", rc == 0);
  check("LLM was called exactly once", fake_summarizer_call_count() == 1);
  check("model was forwarded",
        strcmp(fake_summarizer_last_model(), "test-model") == 0);

  /* len_before = 15, batch_end = min(15 - KEEP_RECENT_MSGS, BATCH=20).
   * After: 1 summary + KEEP_RECENT_MSGS messages remain. */
  int expected_after = 1 + KEEP_RECENT_MSGS;
  check("history collapsed to 1 summary + KEEP_RECENT_MSGS",
        len_before == 15 && ctx_history(ctx)->len == expected_after);
  check("summary lands at index 0 as a user message",
        strcmp(role_at(ctx, 0), "user") == 0);
  check("summary content includes the canned text",
        strstr(content_at(ctx, 0), "Goal: user asked X") != NULL);
  check("last recent message preserved",
        strstr(content_at(ctx, expected_after - 1), "turn-14") != NULL);

  ctx_free(ctx);

  printf("\n=== summary: empty LLM response leaves history intact ===\n");

  fake_summarizer_reset();
  fake_summarizer_set_response(NULL); /* content is NULL */

  ctx = ctx_create(g_config.context_window);
  for (int i = 0; i < 15; i++) {
    snprintf(turn, sizeof(turn), "turn-%d body", i);
    ctx_push(ctx, make_msg(i % 2 == 0 ? "user" : "assistant", turn));
  }
  int snap = ctx_history(ctx)->len;
  rc = summary_policy.apply(ctx, NULL, 0);
  check("apply returned -1 on empty response (handout: must not corrupt)",
        rc == -1);
  check("history unchanged on failure", ctx_history(ctx)->len == snap);
  ctx_free(ctx);

  printf("\n=== summary: short history is a no-op ===\n");

  fake_summarizer_reset();
  fake_summarizer_set_response("ignored");
  ctx = ctx_create(g_config.context_window);
  for (int i = 0; i < KEEP_RECENT_MSGS; i++)
    ctx_push(ctx, make_msg("user", "x"));
  rc = summary_policy.apply(ctx, NULL, 0);
  check("apply returned 0 on short history", rc == 0);
  check("LLM was NOT called on short history",
        fake_summarizer_call_count() == 0);
  check("history unchanged on no-op",
        ctx_history(ctx)->len == KEEP_RECENT_MSGS);
  ctx_free(ctx);

  printf("\n%d / %d passed\n", passed, total);
  return passed == total ? 0 : 1;
}
