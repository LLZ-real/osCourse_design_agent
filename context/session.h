#ifndef CONTEXT_SESSION_H
#define CONTEXT_SESSION_H

/*
 * Session — append‑only log of the conversation on stable storage.
 *
 * Every message pushed into the context is appended to a JSONL file under
 * <workdir>/.agent/session/<name>.log. On restart, replaying the log
 * rebuilds the in-memory conversation so interrupted work can resume.
 */

typedef struct Session Session;

/* Open a session for appending. Creates .agent/session/ if needed. */
Session *session_open(const char *workdir, const char *name);

/* Append one JSON message (as a single line) to the session log. */
void session_append(Session *s, const char *msg_json);

/* Close and free the session handle. */
void session_close(Session *s);

/*
 * Replay a session: read every line in order and call
 * msg_callback(user_data, msg_json) for each valid JSON line.
 * Malformed lines (e.g. a truncated last line after a crash) are skipped.
 * If max_messages > 0, only the most recent max_messages are replayed.
 * Returns the number of valid messages replayed.
 */
int session_replay(const char *workdir, const char *name,
                   void (*msg_callback)(void *user_data, const char *msg),
                   void *user_data, int max_messages);

/* List session names. Returns malloc'd array; caller frees through session_free_list. */
char **session_list(const char *workdir, int *out_count);
void session_free_list(char **list, int count);

/* Delete a session file. */
void session_delete(const char *workdir, const char *name);

#endif /* CONTEXT_SESSION_H */
