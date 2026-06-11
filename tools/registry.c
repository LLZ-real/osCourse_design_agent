/*
 * Tool registry — explicit static registration of built-in tools.
 *
 * The registry is a flat array of ToolDef pointers, populated once at
 * startup and never resized. We chose a flat array (as opposed to a hash
 * map, a linked list, or per-call discovery via constructor functions)
 * because:
 *
 *   - Lookups happen at most a few times per LLM turn; O(N) over a
 *     handful of tools is invisible next to a TCP round-trip.
 *   - A static array is trivial to reason about under TSan: there is no
 *     mutation after tools_init returns.
 *   - There is no hidden machinery — a reader can name every tool the
 *     agent knows by reading tools_init below.
 *
 * If you ever need to load tools dynamically (e.g. plugins discovered at
 * runtime), this is the file that grows. Today it does not.
 */
#include "tools/tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Each tool implementation file declares a single ToolDef as a file-scope
 * static, then exposes it through a non-static "*_def" symbol. The registry
 * names them through the externs below. The handout says explicitly: this
 * is the only file that names every tool by symbol; one new file in tools/
 * plus four lines added here is the full diff for a new tool.
 */
extern ToolDef bash_def;
extern ToolDef read_file_def;
extern ToolDef write_file_def;
extern ToolDef edit_file_def;

static ToolDef *g_tools[MAX_REGISTERED_TOOLS];
static int g_tools_count = 0;

void tools_init(void) {
    if (g_tools_count > 0)
        return;

    tool_register(&bash_def);
    tool_register(&read_file_def);
    tool_register(&write_file_def);
    tool_register(&edit_file_def);
}

void tool_register(ToolDef *def) {
    if (g_tools_count >= MAX_REGISTERED_TOOLS) {
        fprintf(stderr, "Fatal: tool registry full (%d)\n", MAX_REGISTERED_TOOLS);
        exit(1);
    }
    g_tools[g_tools_count++] = def;
}

ToolDef *const *tool_list(int *out_count) {
    if (out_count)
        *out_count = g_tools_count;
    return g_tools;
}

ToolDef *tool_find(const char *name) {
    if (!name)
        return NULL;
    for (int i = 0; i < g_tools_count; i++) {
        if (strcmp(g_tools[i]->name, name) == 0)
            return g_tools[i];
    }
    return NULL;
}

void tool_result_free(ToolResult *r) {
    if (!r)
        return;
    free(r->output);
    r->output = NULL;
}
