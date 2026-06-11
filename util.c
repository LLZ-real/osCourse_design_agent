#include "util.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void die_oom(void) {
  fprintf(stderr, "Fatal: out of memory\n");
  exit(1);
}

void *xmalloc(size_t size) {
  void *p = malloc(size);
  if (!p)
    die_oom();
  return p;
}

void *xrealloc(void *ptr, size_t size) {
  void *p = realloc(ptr, size);
  if (!p)
    die_oom();
  return p;
}

char *xstrdup(const char *s) {
  char *p = strdup(s);
  if (!p)
    die_oom();
  return p;
}

char *xasprintf(const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);

  int len = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (len < 0) {
    va_end(ap2);
    fprintf(stderr, "Fatal: vsnprintf failed\n");
    exit(1);
  }

  char *buf = xmalloc((size_t)len + 1);
  vsnprintf(buf, (size_t)len + 1, fmt, ap2);
  va_end(ap2);
  return buf;
}
