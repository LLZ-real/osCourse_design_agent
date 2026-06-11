#ifndef CONTEXT_INTERNAL_H
#define CONTEXT_INTERNAL_H

#include "cJSON.h"
#include "context/context.h"

#define MAX_POLICIES 8

/* Protect the active tail from reclamation. */
#define KEEP_RECENT_MSGS 4

struct Context {
  MessageList history;
  int context_window;
  int next_offload_id;
  ContextPolicy *policies[MAX_POLICIES];
  int policy_count;
  struct Session *session;   /* non-NULL when session logging is active */
};

extern ContextPolicy offload_policy;
extern ContextPolicy summary_policy;

int ctx_estimate_tokens(const char *msg_json);
int ctx_total_tokens(const Context *ctx);

/*
 * In-place mutations on the message dynarray. Each policy reaches for whichever
 * one matches its semantics:
 *
 *   replace_msg   — body changes, count stays
 *   replace_range — many messages collapse into one
 *
 * replace_msg and replace_range take ownership of the new_json pointer they
 * receive. Callers outside context/ should not be using these.
 */
void ctx_replace_msg(Context *ctx, int index, char *new_json);
void ctx_replace_range(Context *ctx, int from, int to, char *new_json);

/* Get a string field by key, or NULL. */
static inline const char *json_str(cJSON *obj, const char *key) {
  return cJSON_GetStringValue(cJSON_GetObjectItem(obj, key));
}

#endif /* CONTEXT_INTERNAL_H */
