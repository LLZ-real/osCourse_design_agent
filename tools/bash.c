/*
 * bash tool — run a shell command in a forked child and return its output
 * as a ToolResult.
 *
 * The child side (pipe setup, fork, dup2, execl) is given below — you have
 * already written this pattern in Project 1. What the LLM actually needs is
 * the *other half*: after fork, the parent holds pipefd[0] and pid, and
 * must turn that into a ToolResult the agent can send back.
 */
#include "config.h"
#include "tools/tools.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

const char *BASH_TOOL_NAME = "bash";
const char *BASH_TOOL_DESC =
    "Run a shell command and return its combined stdout/stderr.";
const char *BASH_TOOL_SCHEMA =
    "{\"type\":\"object\","
    "\"properties\":{\"command\":{\"type\":\"string\","
    "\"description\":\"The shell command to execute\"}},"
    "\"required\":[\"command\"]}";

ToolDef bash_def = {
    .name = "bash",
    .desc = "Run a shell command and return its combined stdout/stderr.",
    .param_schema = "{\"type\":\"object\","
                    "\"properties\":{\"command\":{\"type\":\"string\","
                    "\"description\":\"The shell command to execute\"}},"
                    "\"required\":[\"command\"]}",
    .exec = bash_tool_exec,
    .read_only = false,
};

static void alarm_handler(int sig) {
    (void)sig;
    extern pid_t g_child_pid;
    extern pid_t g_child_pid;
    if (g_child_pid > 0)
        kill(-g_child_pid, SIGTERM);  /* kill entire process group */
}

ToolResult bash_tool_exec(cJSON *args) {
  const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(args, "command"));
  if (!cmd)
    return (ToolResult){.ok = false,
                        .output = xstrdup("missing 'command' argument")};

  int pipefd[2];
  if (pipe(pipefd) != 0)
    return (ToolResult){
        .ok = false,
        .output = xasprintf("pipe failed: %s", strerror(errno)),
    };

  pid_t pid = fork();
  if (pid < 0) {
    int e = errno;
    close(pipefd[0]);
    close(pipefd[1]);
    return (ToolResult){.ok = false,
                        .output = xasprintf("fork: %s", strerror(e))};
  }

  g_child_pid = pid;

  if (pid == 0) {
    setpgid(0, 0);  /* put child in its own process group */
    close(pipefd[0]);
    /* Set timeout in the child itself — each child owns its own alarm,
       avoiding the process-wide signal races that plague a parent-side
       alarm.  When the alarm fires the child self-terminates, which
       closes the pipe and unblocks the parent's read loop below. */
    if (g_config.tool_timeout > 0) {
        signal(SIGALRM, SIG_DFL);
        alarm(g_config.tool_timeout);
    }
    /* dprintf + _exit avoid stdio-buffer double-flush after fork. */
    if (dup2(pipefd[1], STDOUT_FILENO) < 0 ||
        dup2(pipefd[1], STDERR_FILENO) < 0)
      _exit(127);
    close(pipefd[1]);
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    dprintf(STDERR_FILENO, "exec failed: %s\n", strerror(errno));
    _exit(127);
  }

  close(pipefd[1]);

  /* Parent-side watchdog: if the child doesn't self-terminate in time
     (e.g. a grandchild holds the pipe open), SIGALRM kills the whole
     child process group so the pipe closes.  Set this BEFORE the read
     loop so the timeout actually protects against stuck children. */
  if (g_config.tool_timeout > 0) {
      signal(SIGALRM, alarm_handler);
      alarm(g_config.tool_timeout);
  }

  char *output = NULL;
  size_t output_size = 0;
  char buf[4096];
  for (;;) {
    ssize_t n = read(pipefd[0], buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      int e = errno;
      close(pipefd[0]);
      alarm(0);
      g_child_pid = 0;
      waitpid(pid, NULL, 0);
      free(output);
      return (ToolResult){.ok = false,
                          .output = xasprintf("read failed: %s", strerror(e))};
    }
    if (n == 0)
      break;
    output = xrealloc(output, output_size + (size_t)n + 1);
    memcpy(output + output_size, buf, (size_t)n);
    output_size += (size_t)n;
    output[output_size] = '\0';
  }

  close(pipefd[0]);
  alarm(0);  /* cancel timeout */

  int status = 0;
  if (waitpid(pid, &status, 0) < 0) {
    free(output);
    g_child_pid = 0;
    return (ToolResult){.ok = false,
                        .output = xasprintf("waitpid failed: %s", strerror(errno))};
  }

  g_child_pid = 0;

  if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
    return (ToolResult){.ok = true, .output = output};

  const char *prefix = (output && output[0]) ? output : "";
  char *summary = NULL;
  if (WIFEXITED(status)) {
    summary = xasprintf("%s%sexit %d", prefix, (prefix[0] ? "\n" : ""),
                        WEXITSTATUS(status));
  } else if (WIFSIGNALED(status)) {
    summary = xasprintf("%s%sterminated by signal %d", prefix,
                        (prefix[0] ? "\n" : ""), WTERMSIG(status));
  } else {
    summary = xasprintf("%s%scommand did not exit normally", prefix,
                        (prefix[0] ? "\n" : ""));
  }
  free(output);
  return (ToolResult){.ok = false, .output = summary};
}
