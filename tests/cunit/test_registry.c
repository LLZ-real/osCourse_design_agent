/*
 * tests/cunit/test_registry.c — registry behavior, no threads, no agent.
 *
 * Registers a few stub tools directly via tool_register, then exercises the
 * lookup paths. This is the lowest-level test in the suite; if it fails,
 * everything else is downstream noise.
 */
#include "tools/tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int passed = 0, total = 0;

static void check(const char *name, int ok) {
    total++;
    if (ok) {
        passed++;
        printf("  [PASS] %s\n", name);
    } else {
        printf("  [FAIL] %s\n", name);
    }
}

static ToolResult dummy_exec(cJSON *args) {
    (void)args;
    return (ToolResult){.ok = true, .output = NULL};
}

static ToolDef stub_a = {
    .name = "stub_a",
    .desc = "first stub",
    .param_schema = "{\"type\":\"object\"}",
    .exec = dummy_exec,
};
static ToolDef stub_b = {
    .name = "stub_b",
    .desc = "second stub",
    .param_schema = "{\"type\":\"object\"}",
    .exec = dummy_exec,
};

int main(void) {
    printf("=== Registry: lookup and listing ===\n");

    /* The registry starts empty (a fresh process) — we register two stubs
       manually rather than calling tools_init. */
    tool_register(&stub_a);
    tool_register(&stub_b);

    int n = -1;
    ToolDef *const *list = tool_list(&n);
    check("tool_list count == 2 after two registers", n == 2);
    check("tool_list[0] is stub_a", list[0] == &stub_a);
    check("tool_list[1] is stub_b", list[1] == &stub_b);

    ToolDef *found_a = tool_find("stub_a");
    ToolDef *found_b = tool_find("stub_b");
    ToolDef *found_none = tool_find("nope");

    check("tool_find(stub_a) returns the registered def", found_a == &stub_a);
    check("tool_find(stub_b) returns the registered def", found_b == &stub_b);
    check("tool_find(nope) returns NULL", found_none == NULL);
    check("tool_find(NULL) returns NULL", tool_find(NULL) == NULL);

    /* Re-registering does not collapse: a different ToolDef pointer with the
       same name is preserved (we never claim uniqueness, just ordering). */
    tool_register(&stub_a);
    tool_list(&n);
    check("tool_register is append-only", n == 3);
    check("tool_find returns the FIRST match", tool_find("stub_a") == &stub_a);

    printf("\n%d / %d passed\n", passed, total);
    return passed == total ? 0 : 1;
}
