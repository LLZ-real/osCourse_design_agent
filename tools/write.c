/*
 * write_file tool — write content to a workspace-relative file.
 *
 * Arguments:
 *   path     (required)  Relative path inside the workspace.
 *   content  (required)  Full file contents to write.
 *
 * The parent directory must already exist; tools do NOT create intermediate
 * directories. Returns a message naming the file and byte count on success.
 */
#include "tools/sandbox.h"
#include "tools/tools.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ToolResult tool_write(cJSON *args);

ToolDef write_file_def = {
    .name = "write_file",
    .desc = "Write a file to the workspace, replacing existing content.",
    .param_schema = "{\"type\":\"object\","
                    "\"properties\":{"
                    "\"path\":{\"type\":\"string\","
                    "\"description\":\"Relative path inside the workspace\"},"
                    "\"content\":{\"type\":\"string\","
                    "\"description\":\"Full file contents to write\"}"
                    "},\"required\":[\"path\",\"content\"]}",
    .exec = tool_write,
    .read_only = false,
};

static ToolResult tool_write(cJSON *args) {
    cJSON *path_json = cJSON_GetObjectItem(args, "path");
    cJSON *content_json = cJSON_GetObjectItem(args, "content");

    if (!path_json || !cJSON_IsString(path_json))
        return (ToolResult){.ok = false, .output = xstrdup("missing 'path' argument")};
    if (!content_json || !cJSON_IsString(content_json))
        return (ToolResult){.ok = false, .output = xstrdup("missing 'content' argument")};

    char *resolved = resolve_workspace_path_mkparent(path_json->valuestring);
    if (!resolved)
        return (ToolResult){
            .ok = false,
            .output = xasprintf("path '%s' is outside the workspace", path_json->valuestring),
        };

    FILE *fp = fopen(resolved, "w");
    if (!fp) {
        char *msg = xasprintf("failed to write '%s': %s", resolved, strerror(errno));
        free(resolved);  
        return (ToolResult){.ok = false, .output = msg};
    }

    const char *content = content_json->valuestring;
    size_t len = strlen(content);
    if (fwrite(content, 1, len, fp) != len) {
        char *msg = xasprintf("failed to write '%s': incomplete write", resolved);
        fclose(fp);
        free(resolved);  
        return (ToolResult){.ok = false, .output = msg};
    }

    fclose(fp);
    char *msg = xasprintf("wrote %zu bytes to '%s'", len, resolved);
    free(resolved);  
    return (ToolResult){.ok = true, .output = msg};
}
