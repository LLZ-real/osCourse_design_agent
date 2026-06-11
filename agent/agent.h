#ifndef AGENT_H
#define AGENT_H

/*
 * Opaque agent handle. Early phases can keep only transient per-call state
 * here; Phase C will likely add persistent history for multi-turn dialogue.
 */
typedef struct Agent Agent;

Agent *agent_create(void);
void agent_free(Agent *a);

/*
 * Run one "turn": send user_input to the LLM, execute any tool it asks for,
 * and return the assistant's final text reply. Returned pointer is owned by
 * the agent and is invalidated by the next agent_chat call. Returns NULL on
 * error (and writes a human-readable message via stderr).
 */
const char *agent_chat(Agent *a, const char *user_input);

/*
 * Session management commands.
 * argv[0] is the command name ("list", "save", "load", "new").
 * Returns 0 on success, -1 on unknown command.
 */
int agent_handle_session(Agent *a, int argc, char **argv);

#endif
