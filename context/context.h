#ifndef CONTEXT_H
#define CONTEXT_H

#include "message.h"

#include <stdbool.h>
#include <stddef.h>

/* Opaque Session handle — defined in session.h. */
typedef struct Session Session;

/*
 * Conversation context: the message history plus the policies that keep it
 * within the model's context window.
 */

typedef struct Context Context;

typedef struct {
  const char *name;
  /* True if this policy should run now given current context state. */
  bool (*should_apply)(Context *ctx);
  /* Mutate the context in place. Return 0 on success, -1 on failure.
   * On failure, the callee must write a message into err (up to err_cap
   * bytes). Callers pass an initially-empty buffer and do not have to
   * provide a fallback message. */
  int (*apply)(Context *ctx, char *err, size_t err_cap);
} ContextPolicy;

Context *ctx_create(int context_window);
void ctx_free(Context *ctx);

void ctx_add_policy(Context *ctx, ContextPolicy *policy);

extern ContextPolicy offload_policy;
extern ContextPolicy summary_policy;

void ctx_push(Context *ctx, char *msg_json); /* takes ownership */

/* Attach a session for append-only logging. Takes ownership of `s`. */
void ctx_set_session(Context *ctx, Session *s);

/* Clear all messages from the context history. */
void ctx_clear(Context *ctx);

/*
 * Reclaim context budget: run each triggered policy once. No-op when usage
 * is within thresholds.
 * Returns 0 on success, -1 on failure (err is filled).
 */
int ctx_reclaim(Context *ctx, char *err, size_t err_cap);

float ctx_budget_usage(const Context *ctx); /* fraction in [0,1] */
const MessageList *ctx_history(const Context *ctx);

#endif /* CONTEXT_H */
