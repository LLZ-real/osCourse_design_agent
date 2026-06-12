#ifndef AGENT_H
#define AGENT_H

/*
 * Opaque agent handle. Early phases can keep only transient per-call state
 * here; Phase C will likely add persistent history for multi-turn dialogue.
 */
typedef struct Agent Agent;

/* Context type needed by agent_get_context. */
typedef struct Context Context;

Agent *agent_create(void);
void agent_free(Agent *a);

/*
 * Run one "turn": send user_input to the LLM, execute any tool it asks for,
 * and return the assistant's final text reply. Returned pointer is owned by
 * the agent and is invalidated by the next agent_chat call. Returns NULL on
 * error (and writes a human-readable message via stderr).
 */
const char *agent_chat(Agent *a, const char *user_input);

/* Access the context handle (for REPL session management). */
Context *agent_get_context(Agent *a);

/*
 * Get the metrics from the most recent agent_chat call.
 * Returns a pointer valid until the next agent_chat call.
 */
const char *agent_last_metrics(Agent *a);

#endif
