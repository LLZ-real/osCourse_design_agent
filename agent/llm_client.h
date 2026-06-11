#ifndef LLM_CLIENT_H
#define LLM_CLIENT_H

#include "cJSON.h"
#include "config.h"
#include "message.h"

#include <stddef.h>

typedef struct {
  char *id;    /* opaque LLM-assigned id, echo back with the tool result */
  char *name;  /* tool name the LLM wants invoked */
  cJSON *args; /* parsed arguments object */
} LLMToolCall;

/*
 * Parsed assistant message returned by llm_chat.
 *
 * The fields below are the minimum the agent needs to react to one LLM turn.
 * How you *store* the tool calls — fixed-size array, dynamic array, linked
 * list — is up to you; add whatever fields you need. You are responsible for
 * releasing everything you allocate here (see the cJSON_Delete / free calls
 * your parser will demand).
 */
typedef struct {
  char *content;     /* assistant text, if any (may be NULL or empty) */
  char *raw_message; /* serialized assistant message — push verbatim into
                        history */
  int n_tool_calls;
  LLMToolCall *tool_calls;

  /* Token usage from the API response (0 if unavailable). */
  int prompt_tokens;
  int completion_tokens;
} LLMResponse;

void llm_response_free(LLMResponse *resp);

/*
 * Send a chat-completion request and parse the assistant message.
 *
 *   messages:       the full conversation so far (excluding system prompt)
 *   system_prompt:  string to send as role="system" (may be NULL)
 *   model:          model id
 *   out:            populated on success; caller is responsible for releasing
 *                   anything you allocate inside it
 *   err, err_cap:   error buffer, filled on failure
 *
 * Returns 0 on success, -1 on any error (transport, HTTP status, parse).
 */
int llm_chat(const MessageList *messages, const char *system_prompt,
             const char *model, LLMResponse *out, char *err, size_t err_cap);

#endif
