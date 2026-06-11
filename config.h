#ifndef CONFIG_H
#define CONFIG_H

#include <limits.h>

/*
 * Process-wide runtime configuration, derived once from environment variables.
 */
typedef struct {
  char model[128];
  char llm_host[256];
  char api_key[256];
  int llm_port;
  char workdir[PATH_MAX];
  int max_tokens;

  /* Part 3: context management thresholds. */
  int context_window;
  float offload_threshold;
  float summary_threshold;
} AgentConfig;

extern AgentConfig g_config;

void config_init(void);

#endif
