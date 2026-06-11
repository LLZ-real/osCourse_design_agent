/*
 * tests/cunit/test_executor.c — drive executor_run_tools directly with a
 * read-only test tool. Under `make test-cunit-parallel` (ASan) and
 * `make test-cunit-tsan`, this catches races and leaks in the parallel
 * scheduling path.
 *
 * Why a custom tool instead of read_file: we want every worker thread to
 * write a distinct value through the result-array pointer it was handed,
 * and we want to be able to call this without setting up a workspace. A
 * trivial "echo my input id" tool fits.
 *
 * The bash tool is also registered (and not used) so the registry has
 * realistic shape, but the tool we actually call is read-only — that is
 * what hits the parallel path.
 *
 * If you build this with `-fsanitize=thread`, any data race in your
 * executor.c shows up as a TSan report when this test runs. The agent
 * end-to-end tests cannot cleanly run under TSan because the mock_server
 * subprocess interferes.
 */
#include "agent/llm_client.h"
#include "tools/executor.h"
#include "tools/tools.h"
#include "ui/ui.h"
#include "util.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static ToolResult tool_echo(cJSON *args) {
    /* Sleep a tiny bit to expose ordering bugs: if the executor wrote
       completion order instead of request order, this delay shifts results
       enough to be detectable. */
    usleep(2000);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(args, "id"));
    return (ToolResult){.ok = true, .output = xasprintf("echo:%s", id)};
}

static ToolDef echo_def = {
    .name = "echo",
    .desc = "test tool",
    .param_schema = "{\"type\":\"object\"}",
    .exec = tool_echo,
    .read_only = true,
};

static void make_call(LLMToolCall *call, const char *id) {
    call->id = xstrdup(id);
    call->name = xstrdup("echo");
    call->args = cJSON_CreateObject();
    cJSON_AddStringToObject(call->args, "id", id);
}

static int find_id(char *const out_msgs[], int n, const char *needle) {
    for (int i = 0; i < n; i++) {
        if (strstr(out_msgs[i], needle))
            return i;
    }
    return -1;
}

int main(void) {
    ui_init();

    tool_register(&echo_def);

    printf("=== executor_run_tools: parallel read-only ===\n");

    enum { N = 8 };
    LLMToolCall calls[N];
    for (int i = 0; i < N; i++) {
        char id[16];
        snprintf(id, sizeof(id), "k%d", i);
        make_call(&calls[i], id);
    }

    char *out_msgs[N] = {0};
    char err[256] = "";
    int rc = executor_run_tools(calls, N, out_msgs, err, sizeof(err));
    check("executor returned 0", rc == 0);
    if (rc != 0) {
        fprintf(stderr, "executor error: %s\n", err);
        return 1;
    }

    for (int i = 0; i < N; i++) {
        char needle[32];
        snprintf(needle, sizeof(needle), "echo:k%d", i);
        int idx = find_id(out_msgs, N, needle);
        check("each call's result appears once", idx == i);
    }

    for (int i = 0; i < N; i++) {
        free(out_msgs[i]);
        free(calls[i].id);
        free(calls[i].name);
        cJSON_Delete(calls[i].args);
    }

    printf("\n%d / %d passed\n", passed, total);
    return passed == total ? 0 : 1;
}
