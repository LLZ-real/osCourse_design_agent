#include "fake_summarizer.h"

#include "agent/llm_client.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *g_canned_response = NULL;
static int g_call_count = 0;
static char g_last_model[128];
static int g_force_error = 0;

void fake_summarizer_set_response(const char *content) {
  free(g_canned_response);
  g_canned_response = content ? xstrdup(content) : NULL;
}

void fake_summarizer_force_error(int yes) { g_force_error = yes; }

int fake_summarizer_call_count(void) { return g_call_count; }
const char *fake_summarizer_last_model(void) { return g_last_model; }

void fake_summarizer_reset(void) {
  free(g_canned_response);
  g_canned_response = NULL;
  g_call_count = 0;
  g_force_error = 0;
  g_last_model[0] = '\0';
}

/* ── llm_client symbols ─────────────────────────────────────────────── */

void llm_response_init(LLMResponse *r) { memset(r, 0, sizeof(*r)); }

void llm_response_free(LLMResponse *r) {
  if (!r)
    return;
  free(r->content);
  free(r->raw_message);
  for (int i = 0; i < r->n_tool_calls; i++) {
    free(r->tool_calls[i].id);
    free(r->tool_calls[i].name);
    cJSON_Delete(r->tool_calls[i].args);
  }
  memset(r, 0, sizeof(*r));
}

int llm_chat(const MessageList *messages, const char *system_prompt,
             const char *model, LLMResponse *out, char *err, size_t err_cap) {
  (void)messages;
  (void)system_prompt;

  g_call_count++;
  snprintf(g_last_model, sizeof(g_last_model), "%s", model ? model : "");

  if (g_force_error) {
    snprintf(err, err_cap, "fake_summarizer: forced error");
    return -1;
  }

  llm_response_init(out);
  if (g_canned_response)
    out->content = xstrdup(g_canned_response);
  out->raw_message = xstrdup("{\"role\":\"assistant\",\"content\":\"\"}");
  out->n_tool_calls = 0;
  return 0;
}
