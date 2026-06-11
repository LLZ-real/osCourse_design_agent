#include "config.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

AgentConfig g_config;

static void copy_env_string(char *dst, size_t cap, const char *name,
                            const char *fallback) {
  const char *value = getenv(name);
  snprintf(dst, cap, "%s", (value && value[0]) ? value : fallback);
}

static int parse_env_int(const char *name, int fallback, int min_value,
                         int max_value) {
  const char *value = getenv(name);
  if (!value || !value[0])
    return fallback;
  errno = 0;
  char *end = NULL;
  long parsed = strtol(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed < (long)min_value ||
      parsed > (long)max_value) {
    fprintf(stderr, "[config] warning: invalid %s=%s, using %d\n", name, value,
            fallback);
    return fallback;
  }
  return (int)parsed;
}

static float parse_env_float(const char *name, float fallback,
                             float min_value, float max_value) {
  const char *value = getenv(name);
  if (!value || !value[0])
    return fallback;
  errno = 0;
  char *end = NULL;
  float parsed = strtof(value, &end);
  if (errno != 0 || end == value || *end != '\0' || parsed < min_value ||
      parsed > max_value) {
    fprintf(stderr, "[config] warning: invalid %s=%s, using %.2f\n", name, value,
            (double)fallback);
    return fallback;
  }
  return parsed;
}

void config_init(void) {
  copy_env_string(g_config.model, sizeof(g_config.model), "MODEL_ID",
                  "glm-5.1");
  copy_env_string(g_config.llm_host, sizeof(g_config.llm_host), "LLM_HOST",
                  "127.0.0.1");
  copy_env_string(g_config.api_key, sizeof(g_config.api_key), "API_KEY",
                  "none");

  g_config.llm_port = parse_env_int("LLM_PORT", 18080, 1, 65535);
  g_config.max_tokens = parse_env_int("MAX_TOKENS", 8000, 1, INT_MAX);

  /* Part 3: context management */
  g_config.context_window = parse_env_int("CONTEXT_WINDOW", 32000, 1, INT_MAX);
  g_config.offload_threshold = parse_env_float("OFFLOAD_THRESHOLD", 0.85f, 0.0f, 1.0f);
  g_config.summary_threshold = parse_env_float("SUMMARY_THRESHOLD", 0.95f, 0.0f, 1.0f);

  /* Canonicalize so tools and logs see the same path shape. */
  if (!realpath(".", g_config.workdir)) {
    if (!getcwd(g_config.workdir, sizeof(g_config.workdir))) {
      perror("getcwd");
      exit(1);
    }
  }
}
