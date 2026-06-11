#include "message.h"

#include "cJSON.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

static char *msg_json_with_role(const char *role, const char *content) {
  cJSON *obj = cJSON_CreateObject();
  if (!obj)
    return NULL;
  cJSON_AddStringToObject(obj, "role", role);
  cJSON_AddStringToObject(obj, "content", content ? content : "");
  char *json = cJSON_PrintUnformatted(obj);
  cJSON_Delete(obj);
  return json;
}

void msg_list_init(MessageList *ml) {
  ml->items = NULL;
  ml->len = 0;
  ml->cap = 0;
}

void msg_list_push(MessageList *ml, char *json) {
  if (ml->len >= ml->cap) {
    int new_cap = ml->cap ? ml->cap * 2 : 16;
    ml->items = xrealloc(ml->items, (size_t)new_cap * sizeof(char *));
    ml->cap = new_cap;
  }
  ml->items[ml->len++] = json;
}

void msg_list_free(MessageList *ml) {
  for (int i = 0; i < ml->len; i++)
    free(ml->items[i]);
  free(ml->items);
  ml->items = NULL;
  ml->len = 0;
  ml->cap = 0;
}

char *msg_user_json(const char *content) {
  return msg_json_with_role("user", content);
}

char *msg_tool_json(const char *tool_call_id, const char *content) {
  cJSON *obj = cJSON_CreateObject();
  if (!obj)
    return NULL;
  cJSON_AddStringToObject(obj, "role", "tool");
  cJSON_AddStringToObject(obj, "tool_call_id",
                          tool_call_id ? tool_call_id : "");
  cJSON_AddStringToObject(obj, "content", content ? content : "");
  char *json = cJSON_PrintUnformatted(obj);
  cJSON_Delete(obj);
  return json;
}
