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

  /* Tool execution timeout (seconds). 0 = no timeout. */
  int tool_timeout;

  /* Session replay: max messages to restore on load (0 = no limit). */
  int replay_max;
} AgentConfig;

extern AgentConfig g_config;

void config_init(void);

#endif
