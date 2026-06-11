#include "config.h"
#include "context/context.h"
#include "context/internal.h"
#include "message.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int passed = 0, total = 0;
static char ws[256];

static void check(const char *name, int ok) {
  total++;
  if (ok) {
    passed++;
    printf("  [PASS] %s\n", name);
  } else {
    printf("  [FAIL] %s\n", name);
  }
}

static char *make_msg(const char *role, const char *content,
                      const char *tool_id) {
  cJSON *m = cJSON_CreateObject();
  cJSON_AddStringToObject(m, "role", role);
  cJSON_AddStringToObject(m, "content", content);
  if (tool_id)
    cJSON_AddStringToObject(m, "tool_call_id", tool_id);
  char *out = cJSON_PrintUnformatted(m);
  cJSON_Delete(m);
  return out;
}

static char *make_long(char pad, int n) {
  char *b = malloc((size_t)n + 1);
  memset(b, pad, (size_t)n);
  b[n] = '\0';
  return b;
}

static const char *content_at(const Context *ctx, int i) {
  static char buf[8192];
  cJSON *m = cJSON_Parse(ctx_history(ctx)->items[i]);
  const char *c = json_str(m, "content");
  snprintf(buf, sizeof(buf), "%s", c ? c : "");
  cJSON_Delete(m);
  return buf;
}

static int file_exists(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static long file_size(const char *path) {
  struct stat st;
  if (stat(path, &st) != 0)
    return -1;
  return (long)st.st_size;
}

static void cleanup_workspace(void) {
  char cmd[512];
  if (ws[0]) {
    snprintf(cmd, sizeof(cmd), "rm -rf %s", ws);
    int rc = system(cmd);
    (void)rc;
  }
}

int main(void) {
  snprintf(ws, sizeof(ws), "/tmp/test_offload_%d", (int)getpid());
  mkdir(ws, 0755);
  snprintf(g_config.workdir, sizeof(g_config.workdir), "%s", ws);

  g_config.context_window = 500;
  g_config.offload_threshold = 0.1f; /* always trigger for the test */

  printf("=== offload: long tool messages move to disk, short ones stay ===\n");

  Context *ctx = ctx_create(g_config.context_window);

  /* Non-tool message — must never be offloaded even if long. */
  char *long_payload = make_long('A', 600);
  ctx_push(ctx, make_msg("assistant", long_payload, NULL));

  /* Short tool message — below preview threshold. */
  ctx_push(ctx, make_msg("tool", "small", "call-1"));

  /* Long tool message — the one we expect to land in a file. */
  char *long_tool = make_long('Z', 1200);
  ctx_push(ctx, make_msg("tool", long_tool, "call-2"));

  /* Padding so we are past KEEP_RECENT_MSGS for the in-flight messages. */
  for (int i = 0; i < KEEP_RECENT_MSGS; i++)
    ctx_push(ctx, make_msg("user", "padding", NULL));

  int rc = offload_policy.apply(ctx, NULL, 0);
  check("apply returned 0", rc == 0);

  /* Index 0: assistant with long content, NOT offloaded. */
  check("assistant message untouched",
        strstr(content_at(ctx, 0), "load_storage") == NULL &&
            strstr(content_at(ctx, 0), "read_file") == NULL &&
            strlen(content_at(ctx, 0)) == 600);

  /* Index 1: short tool, untouched. */
  check("short tool message untouched",
        strcmp(content_at(ctx, 1), "small") == 0);

  /* Index 2: long tool, now contains a preview AND a hint mentioning read_file.
   */
  const char *third = content_at(ctx, 2);
  check("long tool message replaced with preview",
        strncmp(third, "ZZZZ", 4) == 0 && strlen(third) < 1200);
  check("preview contains read_file hint", strstr(third, "read_file") != NULL);
  check("preview mentions .agent/offload",
        strstr(third, ".agent/offload/") != NULL);

  /* The offload file exists and has the original payload size. */
  char fpath[512];
  snprintf(fpath, sizeof(fpath), "%s/.agent/offload/0.txt", ws);
  check("offload file created", file_exists(fpath));
  check("offload file holds full payload", file_size(fpath) == 1200);

  printf("\n=== offload: second apply is idempotent ===\n");
  int hist_len_before = ctx_history(ctx)->len;
  rc = offload_policy.apply(ctx, NULL, 0);
  check("second apply returned 0", rc == 0);
  check("history length unchanged", ctx_history(ctx)->len == hist_len_before);

  char fpath2[512];
  snprintf(fpath2, sizeof(fpath2), "%s/.agent/offload/1.txt", ws);
  check("second apply did not create another file", !file_exists(fpath2));

  free(long_payload);
  free(long_tool);
  ctx_free(ctx);

  cleanup_workspace();

  printf("\n%d / %d passed\n", passed, total);
  return passed == total ? 0 : 1;
}
