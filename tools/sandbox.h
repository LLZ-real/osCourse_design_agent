#ifndef TOOLS_SANDBOX_H
#define TOOLS_SANDBOX_H

/*
 * Workspace containment.
 *
 * The agent runs LLM-supplied paths through file tools. A path of the form
 *   "../../etc/passwd"
 * or a symlink that points outside the workdir is something we must catch
 * before any open(2) — otherwise the LLM has a primitive for reading any
 * file the user can read.
 *
 * resolve_workspace_path canonicalizes a relative path and rejects anything
 * that ends up outside g_config.workdir. Returns a heap-allocated absolute
 * path on success (caller frees), or NULL on failure.
 *
 * For paths that do not yet exist (a write target), we resolve the parent
 * directory instead — the parent must exist and be inside the workspace.
 * Tools do NOT create intermediate directories; the LLM is told to call
 * `mkdir -p` via bash if it needs them.
 *
 * This file is framework code. You do not need to modify it. You DO need to
 * use it from every file tool, on every path argument, before opening
 * anything. Look at tools/bash.c for the pattern; replicate it in
 * read.c / write.c / edit.c.
 */
char *resolve_workspace_path(const char *rel_path);

#endif /* TOOLS_SANDBOX_H */
