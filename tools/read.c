/*
 * read_file tool — return workspace-relative file contents to the LLM.
 *
 * Arguments:
 *   path   (required)  Relative path inside the workspace.
 *   limit  (optional)  Maximum number of lines to return.
 *
 * The output is capped at MAX_TOOL_OUTPUT bytes. Every path is validated
 * through resolve_workspace_path before the file is opened.
 */
#include "tools/sandbox.h"
#include "tools/tools.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ToolResult tool_read(cJSON *args);

ToolDef read_file_def = {
    .name = "read_file",
    .desc = "Read a file from the workspace and return its contents.",
    .param_schema = "{\"type\":\"object\","
                    "\"properties\":{"
                    "\"path\":{\"type\":\"string\","
                    "\"description\":\"Relative path inside the workspace\"},"
                    "\"limit\":{\"type\":\"integer\","
                    "\"description\":\"Optional maximum number of lines to return\"}"
                    "},\"required\":[\"path\"]}",
    .exec = tool_read,
    .read_only = true,
};

static ToolResult tool_read(cJSON *args) {
    cJSON *path_json = cJSON_GetObjectItem(args, "path");
    if (!path_json || !cJSON_IsString(path_json))
        return (ToolResult){.ok = false, .output = xstrdup("missing 'path' argument")};

    int limit = 0;
    cJSON *limit_json = cJSON_GetObjectItem(args, "limit");
    if (limit_json && cJSON_IsNumber(limit_json))
        limit = limit_json->valueint;

    char *resolved = resolve_workspace_path(path_json->valuestring);
    if (!resolved)
        return (ToolResult){
            .ok = false,
            .output = xasprintf("path '%s' is outside the workspace or does not exist",
                                path_json->valuestring),
        };

    FILE *fp = fopen(resolved, "r");
    if (!fp) {
        char *msg = xasprintf("failed to read '%s': %s", resolved, strerror(errno));
        free(resolved);
        return (ToolResult){.ok = false, .output = msg};
    }
    free(resolved);

    char buf[MAX_TOOL_OUTPUT + 1];
    size_t total = 0;
    char line[4096];
    int line_count = 0;

    while (fgets(line, sizeof(line), fp)) {
        if (limit > 0 && line_count >= limit)
            break;
        size_t line_len = strlen(line);
        if (total + line_len > MAX_TOOL_OUTPUT) {
            memcpy(buf + total, line, MAX_TOOL_OUTPUT - total);
            total = MAX_TOOL_OUTPUT;
            break;
        }
        memcpy(buf + total, line, line_len);
        total += line_len;
        line_count++;
    }
    buf[total] = '\0';
    fclose(fp);

    return (ToolResult){.ok = true, .output = xstrdup(buf)};
}
