/*
 * Stub tool definitions for CUnit tests that link registry.c
 * but never call tools_init().  These provide the extern symbols
 * that registry.c references so the linker is satisfied.
 */
#include "tools/tools.h"

/* Each stub is a no-op ToolDef with name == NULL so tool_find
   won't match it and tool_list won't crash.  The exec function
   should never be reached because tools_init() is never called
   from the registry/sandbox tests. */

static ToolResult stub_exec(cJSON *a) { (void)a; return (ToolResult){.ok=false,.output=NULL}; }

ToolDef subagent_def =      { .name="__subagent_stub__", .exec=stub_exec };
ToolDef memory_write_def =  { .name="__memwrite_stub__", .exec=stub_exec };
ToolDef memory_read_def =   { .name="__memread_stub__",  .exec=stub_exec };
ToolDef memory_list_def =   { .name="__memlist_stub__",  .exec=stub_exec };
ToolDef skill_load_def =    { .name="__skill_stub__",    .exec=stub_exec };
