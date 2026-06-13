/*
 * SubAgent tool — spawn a child agent with its own context to complete a
 * bounded subtask, then return the result. The sub‑agent runs silently
 * (no UI output) and has its own message history, tools, and turn limit.
 */
#include "agent/llm_client.h"
#include "config.h"
#include "message.h"
#include "tools/executor.h"
#include "tools/tools.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SUBAGENT_MAX_TURNS 8

static const char SUBAGENT_SYSTEM_PROMPT[] =
    "You are a helpful coding sub-agent. Complete the task given to you. "
    "Return a short, final text reply when the task is done.";

static ToolResult tool_subagent(cJSON *args);

ToolDef subagent_def = {
    .name = "subagent",
    .desc = "Spawn a child agent to complete a bounded subtask and return its result.",
    .param_schema = "{\"type\":\"object\","
                    "\"properties\":{"
                    "\"prompt\":{\"type\":\"string\","
                    "\"description\":\"The task for the sub-agent to complete\"}"
                    "},\"required\":[\"prompt\"]}",
    .exec = tool_subagent,
    .read_only = false,
};

static ToolResult tool_subagent(cJSON *args) {
    const char *prompt = cJSON_GetStringValue(cJSON_GetObjectItem(args, "prompt"));
    if (!prompt)
        return (ToolResult){.ok = false, .output = xstrdup("missing 'prompt' argument")};

    MessageList history;
    msg_list_init(&history);

    char *user_json = msg_user_json(prompt);
    if (!user_json) {
        msg_list_free(&history);
        return (ToolResult){.ok = false, .output = xstrdup("failed to create user message")};
    }
    msg_list_push(&history, user_json);

    executor_set_silent(1);

    char err[256];
    int rc = 0;
    ToolResult result = {0};

    for (int turn = 0; turn < SUBAGENT_MAX_TURNS; turn++) {
        LLMResponse response = {0};
        if (llm_chat(&history, SUBAGENT_SYSTEM_PROMPT, g_config.model,
                     &response, err, sizeof(err)) < 0) {
            rc = -1;
            llm_response_free(&response);
            break;
        }

        if (response.raw_message) {
            msg_list_push(&history, response.raw_message);
            response.raw_message = NULL;
        }

        if (response.n_tool_calls == 0) {
            result = (ToolResult){
                .ok = true,
                .output = xstrdup(response.content ? response.content : ""),
            };
            llm_response_free(&response);
            goto done;
        }

        int call_count = response.n_tool_calls;
        char *out_msgs[call_count];
        if (executor_run_tools(response.tool_calls, call_count, out_msgs,
                               err, sizeof(err)) < 0) {
            rc = -1;
            for (int i = 0; i < call_count; i++)
                free(out_msgs[i]);
            llm_response_free(&response);
            break;
        }
        for (int i = 0; i < call_count; i++)
            msg_list_push(&history, out_msgs[i]);
        llm_response_free(&response);
    }

    if (rc != 0) {
        result = (ToolResult){.ok = false, .output = xasprintf("subagent error: %s", err)};
    } else {
        result = (ToolResult){.ok = false, .output = xstrdup("subagent exceeded turn limit")};
    }

done:
    executor_set_silent(0);
    msg_list_free(&history);
    return result;
}
