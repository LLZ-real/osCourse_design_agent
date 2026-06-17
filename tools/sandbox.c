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

char *resolve_workspace_path_mkparent(const char *rel_path) {
  /* First try the standard resolver. */
  char *resolved = resolve_workspace_path(rel_path);
  if (resolved)
    return resolved;

  if (!rel_path || !rel_path[0] || rel_path[0] == '/')
    return NULL;

  /* Build the full path and extract the parent. */
  char full[PATH_MAX];
  snprintf(full, sizeof(full), "%s/%s", g_config.workdir, rel_path);

  char parent[PATH_MAX];
  snprintf(parent, sizeof(parent), "%s", full);
  char *slash = strrchr(parent, '/');
  if (!slash || slash == parent)
    return NULL;
  *slash = '\0';

  /* Check that the parent path is inside the workspace before creating. */
  char resolved_parent[PATH_MAX];
  if (realpath(parent, resolved_parent)) {
    /* Parent already exists — resolve_workspace_path should have worked.
       Something else is wrong; don't create anything. */
    if (within_workspace(resolved_parent)) {
      /* Race: parent appeared between the retry and now.  Try again. */
      free(resolve_workspace_path(rel_path));
      /* Fall through to mkdir path below — harmless if dirs now exist. */
    }
  }

  /* Verify the parent resolves inside the workspace by checking the
     longest-existing prefix.  Walk up from rel_path until we find a
     component that exists and is inside the workspace. */
  char probe[PATH_MAX];
  snprintf(probe, sizeof(probe), "%s", parent);

  while (strlen(probe) > strlen(g_config.workdir)) {
    if (realpath(probe, resolved_parent) && within_workspace(resolved_parent))
      break;
    /* Chop the last component and try again. */
    char *sep = strrchr(probe, '/');
    if (!sep || sep == probe)
      return NULL;
    *sep = '\0';
  }

  /* Create the full parent chain.  This is safe because everything we
     create sits under the verified prefix inside the workspace. */
  snprintf(probe, sizeof(probe), "%s", parent);
  for (char *p = probe + strlen(g_config.workdir) + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(probe, 0755);
      *p = '/';
    }
  }
  mkdir(probe, 0755);

  /* Retry the standard resolver. */
  free(resolved);
  return resolve_workspace_path(rel_path);
}
