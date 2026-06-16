/*
 * llm_client.c — HTTP+JSON glue between the Agent and the LLM service.
 *
 * Your job: implement llm_chat. Everything else in this file is yours to
 * design. You will certainly want helpers (request construction, response
 * parsing, …); whether and how you decompose them is a decision for you.
 */
#include "llm_client.h"

#include "config.h"
#include "http.h"
#include "tools/tools.h"
#include "util.h"


#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Connect + send + recv timeout for LLM requests (seconds).
   Override via LLM_TIMEOUT env var.  Default 60.
   When g_forced_llm_to > 0 it overrides the env var — used by the
   summary policy to set a shorter timeout without changing the API. */
int g_forced_llm_to = 0;

static int llm_timeout_sec(void) {
    if (g_forced_llm_to > 0) return g_forced_llm_to;
    const char *v = getenv("LLM_TIMEOUT");
    if (v && v[0]) {
        long n = strtol(v, NULL, 10);
        if (n > 0 && n < 3600) return (int)n;
    }
    return 60;
}

static void set_err(char *err, size_t err_cap, const char *fmt, ...) {
  if (!err || err_cap == 0)
    return;

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(err, err_cap, fmt, ap);
  va_end(ap);
}

void llm_response_free(LLMResponse *resp) {
  if (!resp)
    return;

  free(resp->content);
  free(resp->raw_message);
  for (int i = 0; i < resp->n_tool_calls; i++) {
    free(resp->tool_calls[i].id);
    free(resp->tool_calls[i].name);
    cJSON_Delete(resp->tool_calls[i].args);
  }
  free(resp->tool_calls);
  memset(resp, 0, sizeof(*resp));
}

int llm_chat(const MessageList *messages, const char *system_prompt,
             const char *model, LLMResponse *out, char *err, size_t err_cap) {
    return llm_chat_with_timeout(messages, system_prompt, model,
                                 llm_timeout_sec(), out, err, err_cap);
}

int llm_chat_with_timeout(const MessageList *messages, const char *system_prompt,
             const char *model, int timeout_sec,
             LLMResponse *out, char *err, size_t err_cap) {
  /*
   * TODO(student, Part 1A):
   * 1. Build the request body. It is a JSON object with fields
   *    `model`, `messages` (system prompt prepended to the given list),
   *    `tools` (one entry describing the bash tool — see BASH_TOOL_NAME /
   *    BASH_TOOL_DESC / BASH_TOOL_SCHEMA in tools/tools.h), and
   *    `max_tokens` (g_config.max_tokens). Use cJSON (see libs/cJSON.h);
   *    hand-splicing strings will not scale.
   *
   * 2. Open a TCP connection (see http.h) and send a POST to
   *    /api/v1/chat/completions with Authorization: Bearer <api_key>.
   *
   * 3. Read the full response with recv_all, parse it with
   *    http_parse_response, and reject any non-200 status.
   *
   * 4. Parse the JSON body. The assistant message lives at
   *    choices[0].message. Extract `content` (may be missing or empty)
   *    and every entry of `tool_calls` (each has `id`, `function.name`,
   *    `function.arguments`). `arguments` arrives as a JSON *string* on
   *    the wire — cJSON_Parse it back into an object. If the string is
   *    empty, treat it as an empty object.
   *
   * 5. Also keep a serialized copy of the whole assistant message
   *    (cJSON_PrintUnformatted is convenient) in out->raw_message — the
   *    agent will push it into history verbatim so the LLM sees its own
   *    previous reply on the next call.
   *
   * Everything you malloc / cJSON_Parse here has to be freed somewhere.
   * Decide where. `make asan` will tell you if you got it wrong.
   */

  memset(out, 0, sizeof(*out));

  char local_err[256];
  char *err_buf = (err && err_cap > 0) ? err : local_err;
  size_t err_buf_cap = (err && err_cap > 0) ? err_cap : sizeof(local_err);

  cJSON *req_json = NULL;
  cJSON *messages_arr = NULL;
  cJSON *tools_arr = NULL;
  cJSON *tool_obj = NULL;
  cJSON *tool_function = NULL;
  cJSON *tool_schema = NULL;
  cJSON *resp_json = NULL;
  char *req_body = NULL;
  char *request = NULL;
  char *resp = NULL;
  const char *resp_body = NULL;
  int sock = -1;
  int status = 0;

  req_json = cJSON_CreateObject();
  if (!req_json) {
    set_err(err_buf, err_buf_cap, "cJSON_CreateObject failed");
    goto error;
  }
  cJSON_AddStringToObject(req_json, "model", model ? model : "");

  messages_arr = cJSON_CreateArray();
  if (!messages_arr) {
    set_err(err_buf, err_buf_cap, "cJSON_CreateArray failed");
    goto error;
  }
  if (system_prompt && system_prompt[0]) {
    cJSON *system_msg = cJSON_CreateObject();
    if (!system_msg) {
      set_err(err_buf, err_buf_cap, "cJSON_CreateObject failed");
      goto error;
    }
    cJSON_AddStringToObject(system_msg, "role", "system");
    cJSON_AddStringToObject(system_msg, "content", system_prompt);
    cJSON_AddItemToArray(messages_arr, system_msg);
  }
  if (messages) {
    for (int i = 0; i < messages->len; i++) {
      cJSON *msg = cJSON_Parse(messages->items[i]);
      if (!msg) {
        set_err(err_buf, err_buf_cap, "cJSON_Parse failed for message %d", i);
        goto error;
      }
      cJSON_AddItemToArray(messages_arr, msg);
    }
  }
  cJSON_AddItemToObject(req_json, "messages", messages_arr);
  messages_arr = NULL;

  tools_arr = cJSON_CreateArray();
  if (!tools_arr) {
    set_err(err_buf, err_buf_cap, "cJSON_CreateArray failed");
    goto error;
  }
  {
    int n_tools;
    ToolDef *const *tools = tool_list(&n_tools);
    for (int i = 0; i < n_tools; i++) {
      tool_obj = cJSON_CreateObject();
      if (!tool_obj) {
        set_err(err_buf, err_buf_cap, "cJSON_CreateObject failed");
        goto error;
      }
      tool_function = cJSON_CreateObject();
      if (!tool_function) {
        set_err(err_buf, err_buf_cap, "cJSON_CreateObject failed");
        goto error;
      }
      cJSON_AddStringToObject(tool_obj, "type", "function");
      cJSON_AddStringToObject(tool_function, "name", tools[i]->name);
      cJSON_AddStringToObject(tool_function, "description", tools[i]->desc);
      tool_schema = cJSON_Parse(tools[i]->param_schema);
      if (!tool_schema) {
        set_err(err_buf, err_buf_cap, "cJSON_Parse failed for tool schema");
        goto error;
      }
      cJSON_AddItemToObject(tool_function, "parameters", tool_schema);
      tool_schema = NULL;
      cJSON_AddItemToObject(tool_obj, "function", tool_function);
      tool_function = NULL;
      cJSON_AddItemToArray(tools_arr, tool_obj);
      tool_obj = NULL;
    }
  }
  cJSON_AddItemToObject(req_json, "tools", tools_arr);
  tools_arr = NULL;
  cJSON_AddNumberToObject(req_json, "max_tokens", g_config.max_tokens);

  req_body = cJSON_PrintUnformatted(req_json);
  if (!req_body) {
    set_err(err_buf, err_buf_cap, "cJSON_PrintUnformatted failed");
    goto error;
  }

  request = xasprintf(
      "POST /api/v1/chat/completions HTTP/1.1\r\n"
      "Host: %s:%d\r\n"
      "Content-Type: application/json\r\n"
      "Authorization: Bearer %s\r\n"
      "Content-Length: %zu\r\n"
      "Connection: close\r\n"
      "\r\n"
      "%s",
      g_config.llm_host, g_config.llm_port, g_config.api_key,
      strlen(req_body), req_body);

  sock = tcp_connect(g_config.llm_host, g_config.llm_port, llm_timeout_sec(),
                       err_buf, err_buf_cap);
  if (sock < 0)
    goto error;

  if (send_all(sock, request, strlen(request)) < 0) {
    set_err(err_buf, err_buf_cap, "send_all failed");
    goto error;
  }

  size_t resp_len = 0;
  if (recv_all(sock, llm_timeout_sec(), &resp, &resp_len, err_buf, err_buf_cap) <
      0) {
    goto error;
  }

  if (http_parse_response(resp, &status, &resp_body) < 0) {
    set_err(err_buf, err_buf_cap, "http_parse_response failed");
    goto error;
  }

  if (status != 200) {
    set_err(err_buf, err_buf_cap, "LLM service returned status %d", status);
    goto error;
  }

  resp_json = cJSON_Parse(resp_body);
  if (!resp_json) {
    set_err(err_buf, err_buf_cap, "cJSON_Parse failed");
    goto error;
  }

  cJSON *choices = cJSON_GetObjectItem(resp_json, "choices");
  if (!choices || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) == 0) {
    set_err(err_buf, err_buf_cap, "Invalid response: missing choices");
    goto error;
  }
  cJSON *choice0 = cJSON_GetArrayItem(choices, 0);
  cJSON *message = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
  if (!message || !cJSON_IsObject(message)) {
    set_err(err_buf, err_buf_cap, "Invalid response: missing message");
    goto error;
  }

  cJSON *content = cJSON_GetObjectItem(message, "content");
  out->content = content && cJSON_IsString(content)
                     ? xstrdup(content->valuestring)
                     : xstrdup("");

  cJSON *tool_calls = cJSON_GetObjectItem(message, "tool_calls");
  if (tool_calls && cJSON_IsArray(tool_calls)) {
    out->n_tool_calls = cJSON_GetArraySize(tool_calls);
    if (out->n_tool_calls > 0) {
      out->tool_calls = calloc((size_t)out->n_tool_calls, sizeof(*out->tool_calls));
      if (!out->tool_calls) {
        set_err(err_buf, err_buf_cap, "calloc failed");
        goto error;
      }
    }
    for (int i = 0; i < out->n_tool_calls; i++) {
      cJSON *call = cJSON_GetArrayItem(tool_calls, i);
      if (!call || !cJSON_IsObject(call)) {
        set_err(err_buf, err_buf_cap, "Invalid response: malformed tool call");
        goto error;
      }
      cJSON *id = cJSON_GetObjectItem(call, "id");
      cJSON *function = cJSON_GetObjectItem(call, "function");
      cJSON *name = function ? cJSON_GetObjectItem(function, "name") : NULL;
      cJSON *arguments = function ? cJSON_GetObjectItem(function, "arguments")
                                  : NULL;
      out->tool_calls[i].id =
          id && cJSON_IsString(id) ? xstrdup(id->valuestring) : xstrdup("");
      out->tool_calls[i].name = name && cJSON_IsString(name)
                                     ? xstrdup(name->valuestring)
                                     : xstrdup("");
      if (arguments && cJSON_IsString(arguments) && arguments->valuestring[0] !=
          '\0') {
        out->tool_calls[i].args = cJSON_Parse(arguments->valuestring);
        if (!out->tool_calls[i].args) {
          set_err(err_buf, err_buf_cap,
                  "cJSON_Parse failed for tool arguments");
          goto error;
        }
      } else {
        out->tool_calls[i].args = cJSON_CreateObject();
        if (!out->tool_calls[i].args) {
          set_err(err_buf, err_buf_cap,
                  "cJSON_CreateObject failed for empty tool arguments");
          goto error;
        }
      }
    }
  } else {
    out->tool_calls = NULL;
    out->n_tool_calls = 0;
  }

  /* Parse token usage from the API response. */
  cJSON *usage = cJSON_GetObjectItem(resp_json, "usage");
  if (usage && cJSON_IsObject(usage)) {
    cJSON *pt = cJSON_GetObjectItem(usage, "prompt_tokens");
    cJSON *ct = cJSON_GetObjectItem(usage, "completion_tokens");
    out->prompt_tokens = pt && cJSON_IsNumber(pt) ? pt->valueint : 0;
    out->completion_tokens = ct && cJSON_IsNumber(ct) ? ct->valueint : 0;
  }

  out->raw_message = cJSON_PrintUnformatted(message);
  if (!out->raw_message) {
    set_err(err_buf, err_buf_cap, "cJSON_PrintUnformatted failed for raw_message");
    goto error;
  }

  cJSON_Delete(resp_json);
  free(resp);
  free(request);
  free(req_body);
  if (sock >= 0)
    close(sock);
  cJSON_Delete(req_json);
  return 0;

error:
  llm_response_free(out);
  cJSON_Delete(resp_json);
  /* Clean up intermediate cJSON objects that may not yet be attached
     to req_json when the error occurred.  cJSON_Delete(NULL) is safe. */
  cJSON_Delete(tool_schema);
  cJSON_Delete(tool_function);
  cJSON_Delete(tool_obj);
  cJSON_Delete(tools_arr);
  cJSON_Delete(messages_arr);
  free(resp);
  free(request);
  free(req_body);
  if (sock >= 0)
    close(sock);
  cJSON_Delete(req_json);
  return -1;
}
