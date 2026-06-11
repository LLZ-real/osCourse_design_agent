/*
 * util.h — allocation wrappers that terminate on failure.
 *
 * A CLI tool cannot meaningfully recover from out-of-memory — every subsequent
 * allocation would fail too. These wrappers make the policy explicit and
 * remove repetitive NULL checks from the rest of the codebase. The 'x' prefix
 * is a Unix convention (git, openssh) meaning "exit on failure".
 */
#ifndef UTIL_H
#define UTIL_H

#include <stddef.h>

void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
char *xstrdup(const char *s);

/* Like asprintf but exits on failure. Never returns NULL. */
char *xasprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#endif
