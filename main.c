#include "agent/agent.h"
#include "config.h"
#include "context/context.h"
#include "tools/tools.h"
#include "ui/ui.h"

#include <readline/history.h>
#include <readline/readline.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int handle_session(Agent *a, const char *input) {
  /* Skip "/session" prefix. */
  const char *rest = input + 8;
  while (*rest == ' ') rest++;

  if (*rest == '\0') {
    /* Bare "/session" — show usage. */
    agent_handle_session(a, 0, NULL);
    return 1;
  }

  /* Split the rest into argv[]. */
  char *copy = strdup(rest);
  if (!copy) return 1;

  char *argv[16];
  int argc = 0;
  char *tok = strtok(copy, " ");
  while (tok && argc < 16) {
    argv[argc++] = tok;
    tok = strtok(NULL, " ");
  }

  agent_handle_session(a, argc, argv);
  free(copy);
  return 1;
}

int main(void) {
  config_init();
  tools_init();
  ui_init();
  ui_start();

  Agent *a = agent_create();
  if (!a) {
    fprintf(stderr, "agent_create failed\n");
    ui_stop();
    return 1;
  }

  ui_banner();

  int rc = 0;

  for (;;) {
    char *input = readline("> ");
    if (!input)
      break;

    if (strcmp(input, "exit") == 0 || strcmp(input, "quit") == 0 ||
        strcmp(input, "q") == 0) {
      free(input);
      break;
    }

    /* Handle `/session` commands. */
    if (strncmp(input, "/session", 8) == 0) {
      handle_session(a, input);
      free(input);
      continue;
    }

    if (input[0] != '\0')
      add_history(input);

    const char *reply = agent_chat(a, input);
    free(input);

    if (reply) {
      ui_idle();
      printf("%s\n", reply);
    } else {
      rc = 1;
    }
  }

  agent_free(a);
  ui_stop();
  return rc;
}
