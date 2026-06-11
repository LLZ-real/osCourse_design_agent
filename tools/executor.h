#ifndef TOOLS_EXECUTOR_H
#define TOOLS_EXECUTOR_H

#include "agent/llm_client.h"

#include <stddef.h>

/*
 * Run every tool the LLM asked for in one assistant message.
 *
 * Inputs:
 *   tool_calls   array of LLMToolCall, in the order the LLM emitted them
 *   count        size of that array (0 < count <= MAX_TOOL_CALLS)
 *
 * Outputs:
 *   out_msgs     array of `count` malloc'd strings; on success out_msgs[i]
 *                is a serialized {"role":"tool",...} JSON message ready to
 *                push into history. Caller takes ownership.
 *
 *   err          error buffer, written only on failure
 *
 * Returns 0 on success, -1 on a setup or serialization failure. Tool
 * EXECUTION failures (a bash exit code, an unknown tool name) are *not*
 * errors at this layer — they are encoded in the corresponding tool message
 * and fed back to the LLM.
 *
 * Ordering invariant: out_msgs[i] must correspond to tool_calls[i] regardless
 * of the order in which tools complete. Phase A trivially satisfies this
 * (the loop runs serially in request order). Phase B has to keep it true
 * even when independent reads finish out of order.
 */
int executor_run_tools(
    LLMToolCall tool_calls[], int count, char *out_msgs[], char *err, size_t err_cap
);

#endif /* TOOLS_EXECUTOR_H */
