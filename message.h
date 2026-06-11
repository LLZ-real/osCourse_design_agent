#ifndef MESSAGE_H
#define MESSAGE_H

/*
 * Dynamic array of serialized chat messages. Each entry is a heap-allocated
 * JSON string (e.g. {"role":"user","content":"..."}). The list is the core
 * data structure the Agent builds on: user/assistant/tool messages accumulate
 * here and are sent back to the LLM on every request.
 */
typedef struct {
  char **items;
  int len;
  int cap;
} MessageList;

void msg_list_init(MessageList *ml);
void msg_list_push(MessageList *ml, char *json); /* takes ownership */
void msg_list_free(MessageList *ml);

/* Construct message JSON strings (malloc'd, caller owns). */
char *msg_user_json(const char *content);
char *msg_tool_json(const char *tool_call_id, const char *content);

#endif
