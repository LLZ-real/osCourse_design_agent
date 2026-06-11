/*
 * Workspace containment — see sandbox.h for the threat model.
 *
 * Why we use realpath(3) and not a string-level "does it contain ..":
 *
 *   - "foo/../../etc/passwd" passes a naive ".." check because the ".." is
 *     in the middle, but it still escapes after canonicalization.
 *   - A symlink can point anywhere without any ".." in the path string.
 *   - Even a perfectly innocent absolute path like "/tmp/agent/x" might
 *     not be inside the agent's workdir.
 *
 * realpath resolves symlinks and ".." against the actual filesystem, so by
 * the time we have its output we know exactly what file the kernel will
 * open. If that location is not under g_config.workdir, we refuse.
 *
 * Containment uses a prefix check followed by a separator check: the workdir
 * "/foo/bar" must not match the path "/foo/bar-evil" — they share the
 * "/foo/bar" prefix but live in different directories. We require either a
 * '/' or NUL immediately after the prefix.
 *
 * The "file does not exist yet" case (write target) gets the parent-dir
 * realpath treatment: the parent must exist and be inside; the leaf is
 * grafted on by string concatenation. We do NOT create the parent — that
 * keeps the security surface small (writes never silently mkdir into
 * paths the LLM has not explicitly arranged for).
 */
#include "tools/sandbox.h"

#include "config.h"
#include "util.h"

#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool within_workspace(const char *resolved) {
    const char *workdir = g_config.workdir;
    size_t n = strlen(workdir);
    return strncmp(resolved, workdir, n) == 0 && (resolved[n] == '/' || resolved[n] == '\0');
}

char *resolve_workspace_path(const char *rel_path) {
    if (!rel_path || !rel_path[0] || rel_path[0] == '/')
        return NULL;

    char full[PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", g_config.workdir, rel_path);

    char resolved[PATH_MAX];
    if (realpath(full, resolved))
        return within_workspace(resolved) ? xstrdup(resolved) : NULL;

    /* Path does not exist yet — resolve the parent and graft the leaf back on. */
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", full);
    char *slash = strrchr(parent, '/');
    if (!slash || slash == parent)
        return NULL;
    *slash = '\0';
    const char *leaf = slash + 1;

    char resolved_parent[PATH_MAX];
    if (!realpath(parent, resolved_parent))
        return NULL;
    if (!within_workspace(resolved_parent))
        return NULL;

    return xasprintf("%s/%s", resolved_parent, leaf);
}
