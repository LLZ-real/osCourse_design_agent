#ifndef TOOLS_H
#define TOOLS_H

#include "cJSON.h"

#include <stdbool.h>

/*
 * Part 1 types — kept for backward compatibility.
 */
typedef struct {
  bool ok;      /* command exited cleanly / tool succeeded */
  char *output; /* malloc'd result text */
} ToolResult;

void tool_result_free(ToolResult *r);

/* ── Part 2: common tool interface ──────────────────── */

#define MAX_TOOL_OUTPUT 50000
#define MAX_REGISTERED_TOOLS 16

typedef ToolResult (*ToolFn)(cJSON *args);

typedef struct {
    const char *name;
    const char *desc;
    const char *param_schema;
    ToolFn exec;
    bool read_only;
} ToolDef;

/* Registry — lifetime: call tools_init() once at startup. */
void tools_init(void);
void tool_register(ToolDef *def);
ToolDef *tool_find(const char *name);
ToolDef *const *tool_list(int *out_count);

/* ── bash (Part 1 carry-over) ───────────────────────── */

extern const char *BASH_TOOL_NAME;
extern const char *BASH_TOOL_DESC;
extern const char *BASH_TOOL_SCHEMA;

ToolResult bash_tool_exec(cJSON *args);

/* Skill system: build listing of available skills for the system prompt. */
char *skill_build_listing(void);

#endif
