/*
 * edit_file tool — replace the first exact occurrence of old_text with
 * new_text in a workspace-relative file.
 *
 * Arguments:
 *   path      (required)  Relative path inside the workspace.
 *   old_text  (required)  Exact substring to find.
 *   new_text  (required)  Replacement text.
 *
 * If old_text is not found, the tool fails with a descriptive message.
 */
#include "tools/sandbox.h"
#include "tools/tools.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ToolResult tool_edit(cJSON *args);

ToolDef edit_file_def = {
    .name = "edit_file",
    .desc = "Replace the first exact match of old_text with new_text in a file.",
    .param_schema = "{\"type\":\"object\","
                    "\"properties\":{"
                    "\"path\":{\"type\":\"string\","
                    "\"description\":\"Relative path inside the workspace\"},"
                    "\"old_text\":{\"type\":\"string\","
                    "\"description\":\"Exact substring to find\"},"
                    "\"new_text\":{\"type\":\"string\","
                    "\"description\":\"Replacement text\"}"
                    "},\"required\":[\"path\",\"old_text\",\"new_text\"]}",
    .exec = tool_edit,
    .read_only = false,
};

static ToolResult tool_edit(cJSON *args) {
    cJSON *path_json = cJSON_GetObjectItem(args, "path");
    cJSON *old_text_json = cJSON_GetObjectItem(args, "old_text");
    cJSON *new_text_json = cJSON_GetObjectItem(args, "new_text");

    if (!path_json || !cJSON_IsString(path_json))
        return (ToolResult){.ok = false, .output = xstrdup("missing 'path' argument")};
    if (!old_text_json || !cJSON_IsString(old_text_json))
        return (ToolResult){.ok = false, .output = xstrdup("missing 'old_text' argument")};
    if (!new_text_json || !cJSON_IsString(new_text_json))
        return (ToolResult){.ok = false, .output = xstrdup("missing 'new_text' argument")};

    char *resolved = resolve_workspace_path(path_json->valuestring);
    if (!resolved)
        return (ToolResult){
            .ok = false,
            .output = xasprintf("path '%s' is outside the workspace or does not exist",
                                path_json->valuestring),
        };

    /* Read the entire file. */
    FILE *fp = fopen(resolved, "r");
    if (!fp) {
        char *msg = xasprintf("failed to read '%s': %s", resolved, strerror(errno));
        free(resolved);
        return (ToolResult){.ok = false, .output = msg};
    }

    fseek(fp, 0, SEEK_END);
    long file_size = ftell(fp);
    if (file_size < 0) {
        char *msg = xasprintf("failed to read '%s'", resolved);
        fclose(fp);
        free(resolved);
        return (ToolResult){.ok = false, .output = msg};
    }
    rewind(fp);

    char *content = xmalloc((size_t)file_size + 1);
    size_t nread = fread(content, 1, (size_t)file_size, fp);
    content[nread] = '\0';
    fclose(fp);

    /* Find the first exact match. */
    const char *old_text = old_text_json->valuestring;
    const char *new_text = new_text_json->valuestring;
    size_t old_len = strlen(old_text);

    char *match = strstr(content, old_text);
    if (!match) {
        char *msg = xasprintf("edit failed: substring not found in '%s'", resolved);
        free(content);
        free(resolved);
        return (ToolResult){.ok = false, .output = msg};
    }

    /* Build the new content. */
    size_t offset = (size_t)(match - content);
    size_t new_len = strlen(new_text);
    size_t content_len = strlen(content);
    size_t result_len = content_len - old_len + new_len;
    char *result = xmalloc(result_len + 1);

    memcpy(result, content, offset);
    memcpy(result + offset, new_text, new_len);
    memcpy(result + offset + new_len, content + offset + old_len,
           content_len - offset - old_len + 1);
    free(content);

    /* Write back. */
    fp = fopen(resolved, "w");
    if (!fp) {
        char *msg = xasprintf("failed to write '%s': %s", resolved, strerror(errno));
        free(result);
        free(resolved);
        return (ToolResult){.ok = false, .output = msg};
    }

    if (fwrite(result, 1, result_len, fp) != result_len) {
        char *msg = xasprintf("failed to write '%s'", resolved);
        fclose(fp);
        free(result);
        free(resolved);
        return (ToolResult){.ok = false, .output = msg};
    }
    fclose(fp);

    char *msg = xasprintf("updated '%s'", resolved);
    free(result);
    free(resolved);
    return (ToolResult){.ok = true, .output = msg};
}
