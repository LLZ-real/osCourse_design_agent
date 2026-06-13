/*
 * Memory tools — persistent key-value store at <workdir>/.agent/memory/.
 *
 * The agent can write observations, project conventions, and decisions
 * that survive session restarts. Each entry is a JSON file.
 *
 * Tools: memory_write, memory_read, memory_list
 */
#include "config.h"
#include "tools/sandbox.h"
#include "tools/tools.h"
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

/* ── Helpers ─────────────────────────────────────────────────────────── */

static void ensure_memory_dir(void) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/.agent", g_config.workdir);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/.agent/memory", g_config.workdir);
    mkdir(path, 0755);
}

/* Validate a memory key: only alphanumeric, dash, underscore, dot. */
static int valid_key(const char *key) {
    if (!key || !key[0])
        return 0;
    for (const char *p = key; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.'))
            return 0;
    }
    return 1;
}

/* ── memory_write ────────────────────────────────────────────────────── */

static ToolResult tool_memory_write(cJSON *args);

ToolDef memory_write_def = {
    .name = "memory_write",
    .desc = "Save a memory entry that persists across sessions.",
    .param_schema = "{\"type\":\"object\","
                    "\"properties\":{"
                    "\"key\":{\"type\":\"string\","
                    "\"description\":\"Memory key (alphanumeric, dash, underscore, dot)\"},"
                    "\"content\":{\"type\":\"string\","
                    "\"description\":\"Content to remember\"}"
                    "},\"required\":[\"key\",\"content\"]}",
    .exec = tool_memory_write,
    .read_only = false,
};

static ToolResult tool_memory_write(cJSON *args) {
    cJSON *key_j = cJSON_GetObjectItem(args, "key");
    cJSON *content_j = cJSON_GetObjectItem(args, "content");

    if (!key_j || !cJSON_IsString(key_j))
        return (ToolResult){.ok = false, .output = xstrdup("missing 'key' argument")};
    if (!content_j || !cJSON_IsString(content_j))
        return (ToolResult){.ok = false, .output = xstrdup("missing 'content' argument")};

    const char *key = key_j->valuestring;
    const char *content = content_j->valuestring;

    if (!valid_key(key))
        return (ToolResult){.ok = false, .output = xasprintf("invalid key '%s': use alphanumeric, dash, underscore, dot", key)};

    ensure_memory_dir();

    char path[4096];
    snprintf(path, sizeof(path), "%s/.agent/memory/%s.json", g_config.workdir, key);

    /* Build JSON with timestamp. */
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", t);

    cJSON *obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "key", key);
    cJSON_AddStringToObject(obj, "content", content);
    cJSON_AddStringToObject(obj, "timestamp", ts);
    char *json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        free(json);
        return (ToolResult){.ok = false, .output = xasprintf("failed to write memory '%s'", key)};
    }
    fprintf(fp, "%s\n", json);
    fclose(fp);
    free(json);

    return (ToolResult){.ok = true, .output = xasprintf("saved memory '%s' (%zu bytes)", key, strlen(content))};
}

/* ── memory_read ─────────────────────────────────────────────────────── */

static ToolResult tool_memory_read(cJSON *args);

ToolDef memory_read_def = {
    .name = "memory_read",
    .desc = "Read a saved memory entry by key.",
    .param_schema = "{\"type\":\"object\","
                    "\"properties\":{"
                    "\"key\":{\"type\":\"string\","
                    "\"description\":\"Memory key to read\"}"
                    "},\"required\":[\"key\"]}",
    .exec = tool_memory_read,
    .read_only = true,
};

static ToolResult tool_memory_read(cJSON *args) {
    cJSON *key_j = cJSON_GetObjectItem(args, "key");
    if (!key_j || !cJSON_IsString(key_j))
        return (ToolResult){.ok = false, .output = xstrdup("missing 'key' argument")};

    const char *key = key_j->valuestring;
    if (!valid_key(key))
        return (ToolResult){.ok = false, .output = xasprintf("invalid key '%s'", key)};

    char path[4096];
    snprintf(path, sizeof(path), "%s/.agent/memory/%s.json", g_config.workdir, key);

    FILE *fp = fopen(path, "r");
    if (!fp)
        return (ToolResult){.ok = false, .output = xasprintf("memory '%s' not found", key)};

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    char *buf = xmalloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, fp);
    buf[sz] = '\0';
    fclose(fp);

    /* Parse to extract content. */
    cJSON *obj = cJSON_Parse(buf);
    free(buf);
    if (!obj) {
        return (ToolResult){.ok = false, .output = xasprintf("failed to parse memory '%s'", key)};
    }

    cJSON *content_j = cJSON_GetObjectItem(obj, "content");
    const char *content = content_j && cJSON_IsString(content_j) ? content_j->valuestring : "(empty)";
    char *result = xstrdup(content);
    cJSON_Delete(obj);

    return (ToolResult){.ok = true, .output = result};
}

/* ── memory_list ─────────────────────────────────────────────────────── */

static ToolResult tool_memory_list(cJSON *args);

ToolDef memory_list_def = {
    .name = "memory_list",
    .desc = "List all saved memory entries with keys and summaries.",
    .param_schema = "{\"type\":\"object\",\"properties\":{},\"required\":[]}",
    .exec = tool_memory_list,
    .read_only = true,
};

static ToolResult tool_memory_list(cJSON *args) {
    (void)args;

    char dir_path[4096];
    snprintf(dir_path, sizeof(dir_path), "%s/.agent/memory", g_config.workdir);

    DIR *d = opendir(dir_path);
    if (!d)
        return (ToolResult){.ok = true, .output = xstrdup("No memory entries found.")};

    /* Collect entries. */
    int cap = 16, count = 0;
    char **entries = xmalloc((size_t)cap * sizeof(char *));

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".json") != 0)
            continue;

        char path[4096];
        snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;

        char line[4096];
        if (fgets(line, sizeof(line), fp)) {
            cJSON *obj = cJSON_Parse(line);
            if (obj) {
                cJSON *c_j = cJSON_GetObjectItem(obj, "content");
                cJSON *t_j = cJSON_GetObjectItem(obj, "timestamp");
                const char *content = c_j && cJSON_IsString(c_j) ? c_j->valuestring : "";
                const char *ts = t_j && cJSON_IsString(t_j) ? t_j->valuestring : "";
                /* Show the key name (without .json extension). */
                size_t nlen = (size_t)(dot - entry->d_name);
                char preview[128];
                snprintf(preview, sizeof(preview), "  %.*s (%.50s...) [%s]", (int)nlen, entry->d_name, content, ts);
                if (count >= cap) {
                    cap *= 2;
                    entries = xrealloc(entries, (size_t)cap * sizeof(char *));
                }
                entries[count++] = xstrdup(preview);
                cJSON_Delete(obj);
            }
        }
        fclose(fp);
    }
    closedir(d);

    if (count == 0) {
        free(entries);
        return (ToolResult){.ok = true, .output = xstrdup("No memory entries found.")};
    }

    /* Build output. */
    size_t total = 0;
    for (int i = 0; i < count; i++)
        total += strlen(entries[i]) + 1;

    char *out = xmalloc(total + 1);
    out[0] = '\0';
    for (int i = 0; i < count; i++) {
        strcat(out, entries[i]);
        strcat(out, "\n");
        free(entries[i]);
    }
    free(entries);

    return (ToolResult){.ok = true, .output = out};
}
