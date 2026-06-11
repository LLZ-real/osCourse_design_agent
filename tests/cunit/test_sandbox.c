/*
 * tests/cunit/test_sandbox.c — workspace path containment, no threads.
 *
 * resolve_workspace_path is the security boundary for every file tool. Every
 * change to the path validation logic must keep these checks green.
 */
#include "config.h"
#include "tools/sandbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int passed = 0, total = 0;
static char workspace_dir[256];

static void check(const char *name, int ok) {
    total++;
    if (ok) {
        passed++;
        printf("  [PASS] %s\n", name);
    } else {
        printf("  [FAIL] %s\n", name);
    }
}

static int prefix_eq(const char *a, const char *b) { return a && strncmp(a, b, strlen(b)) == 0; }

int main(void) {
    /* Create a temp workspace so the sandbox has somewhere to point at. */
    snprintf(workspace_dir, sizeof(workspace_dir), "/tmp/sandbox_test_%d", (int)getpid());
    mkdir(workspace_dir, 0700);
    /* Create a real file inside so realpath-on-existing-leaf can succeed. */
    char inside_path[512];
    snprintf(inside_path, sizeof(inside_path), "%s/inside.txt", workspace_dir);
    FILE *fp = fopen(inside_path, "w");
    fprintf(fp, "x");
    fclose(fp);

    /* config_init uses realpath(".") for workdir, so chdir into the workspace
       before initializing the global. */
    if (chdir(workspace_dir) != 0) {
        perror("chdir");
        return 1;
    }
    config_init();

    printf("=== Sandbox: paths inside the workspace ===\n");

    char *p = resolve_workspace_path("inside.txt");
    check("relative path inside is accepted", p != NULL);
    check("inside path resolves under workdir", prefix_eq(p, g_config.workdir));
    free(p);

    p = resolve_workspace_path("does_not_exist_yet.txt");
    check("non-existent leaf accepted (parent inside)", p != NULL);
    free(p);

    printf("\n=== Sandbox: rejection cases ===\n");

    check("absolute path rejected", resolve_workspace_path("/etc/passwd") == NULL);
    check("../escape rejected", resolve_workspace_path("../../etc/passwd") == NULL);
    check("NULL path rejected", resolve_workspace_path(NULL) == NULL);
    check(
        "empty path rejected (parent is workspace dir itself, leaf empty)",
        resolve_workspace_path("") == NULL
    );

    /* Cleanup. */
    unlink(inside_path);
    rmdir(workspace_dir);

    printf("\n%d / %d passed\n", passed, total);
    return passed == total ? 0 : 1;
}
