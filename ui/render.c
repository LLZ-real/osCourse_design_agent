/*
 * Render thread: a dedicated thread that owns all terminal redraws.
 *
 * Producer-consumer pattern: the main thread and tool worker threads push
 * events into a queue protected by g_ui.mutex; the render thread drains them
 * at a small fixed cadence. This avoids interleaved terminal output when
 * multiple threads complete concurrently.
 *
 * The contents of this file are framework code. The public contract lives
 * in ui/ui.h; if you only care about the contract, stop there.
 */
#include "ui/internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/* ── Output sanitization ───────────────────────────── */

/*
 * Strip ANSI CSI escapes (\033[...letter) and C0 controls from tool output
 * so they don't corrupt the terminal layout. Keeps newlines; expands tabs.
 */
static size_t sanitize_output(const char *raw, char *clean, size_t cap) {
  size_t ri = 0, wi = 0;
  while (raw[ri] && wi < cap - 1) {
    unsigned char c = (unsigned char)raw[ri];
    if (c == '\033' && raw[ri + 1] == '[') {
      ri += 2;
      while (raw[ri] && (raw[ri] < '@' || raw[ri] > '~'))
        ri++;
      if (raw[ri])
        ri++;
    } else if (c == '\t') {
      for (int i = 0; i < 4 && wi < cap - 1; i++)
        clean[wi++] = ' ';
      ri++;
    } else if (c == '\n' || c >= 0x20) {
      clean[wi++] = (char)c;
      ri++;
    } else {
      ri++; /* drop CR, bell, backspace, other C0 */
    }
  }
  clean[wi] = '\0';
  return wi;
}

/* ── Spinner ──────────────────────────────────────── */

static const char *SPINNER[] = {
    "\xe2\xa0\x8b", "\xe2\xa0\x99", "\xe2\xa0\xb9", "\xe2\xa0\xb8",
    "\xe2\xa0\xbc", "\xe2\xa0\xb4", "\xe2\xa0\xa6", "\xe2\xa0\xa7",
    "\xe2\xa0\x87", "\xe2\xa0\x8f",
};
#define SPINNER_COUNT 10

/* ── Terminal helpers ─────────────────────────────── */

static double ts_diff(struct timespec a, struct timespec b) {
  return (double)(b.tv_sec - a.tv_sec) + (double)(b.tv_nsec - a.tv_nsec) / 1e9;
}

static int terminal_columns(void) {
  struct winsize ws;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    return ws.ws_col;
  return 80;
}

static void print_clipped(const char *text, int len, int max_cols) {
  if (!text || len <= 0 || max_cols <= 0)
    return;
  if (len <= max_cols)
    printf("%.*s", len, text);
  else if (max_cols <= 3)
    printf("%.*s", max_cols, text);
  else
    printf("%.*s...", max_cols - 3, text);
}

/*
 * Tool names are registry literals and args are cJSON_PrintUnformatted
 * output — both are single-line and contain no ANSI, so no sanitizing.
 */
static void print_tool_label(const ToolSlot *s, int max_cols) {
  if (max_cols <= 0)
    return;

  const char *name = (s->name && s->name[0]) ? s->name : "(unknown)";
  int name_len = (int)strlen(name);
  print_clipped(name, name_len, max_cols);

  if (!s->args_display || !s->args_display[0])
    return;

  int arg_cols = max_cols - name_len - 3; /* space + 2 backticks */
  if (arg_cols <= 0)
    return;

  printf(" ");
  esc(ESC_DIM);
  print_clipped(s->args_display, (int)strlen(s->args_display), arg_cols);
  esc(ESC_RESET);
}

/* ── Result rendering ──────────────────────────────── */

#define MAX_RESULT_LINES 8

static int render_result_lines(const char *result) {
  if (!result || !*result || strcmp(result, "(no output)") == 0)
    return 0;

  size_t rlen = strlen(result);
  char *clean = malloc(rlen + 1);
  if (!clean)
    return 0;
  size_t clen = sanitize_output(result, clean, rlen + 1);

  /* Trim leading and trailing newlines. */
  const char *body = clean;
  while (*body == '\n')
    body++;
  while (clen > 0 && clean[clen - 1] == '\n')
    clean[--clen] = '\0';
  if (!*body) {
    free(clean);
    return 0;
  }

  int max_cols = terminal_columns() - 4; /* "  │ " prefix */
  if (max_cols < 1)
    max_cols = 1;

  esc(ESC_GRAY);
  const char *p = body;
  int shown = 0, printed = 0;
  while (*p && shown < MAX_RESULT_LINES) {
    const char *nl = strchr(p, '\n');
    int len = nl ? (int)(nl - p) : (int)strlen(p);
    printf("  \xe2\x94\x82 ");
    print_clipped(p, len, max_cols);
    printf("\n");
    printed++;
    shown++;
    p = nl ? nl + 1 : p + len;
  }

  /* Count any remaining lines past the cap. */
  int extra = 0;
  for (const char *q = p; *q; q++)
    if (*q == '\n')
      extra++;
  if (*p)
    extra++;
  if (extra > 0) {
    printf("  \xe2\x94\x82 ... (%d more lines)\n", extra);
    printed++;
  }
  esc(ESC_RESET);

  free(clean);
  return printed;
}

/* ── Dynamic region ───────────────────────────────── */

static void erase_dynamic(UIState *ui) {
  if (ui->lines_drawn > 0) {
    printf("\033[%dA\033[J", ui->lines_drawn);
    ui->lines_drawn = 0;
    fflush(stdout);
  }
}

/* ── Render frames ────────────────────────────────── */

static void render_thinking(UIState *ui, int frame) {
  erase_dynamic(ui);
  esc(ESC_GREEN);
  printf("%s ", SPINNER[frame % SPINNER_COUNT]);
  esc(ESC_RESET);
  esc(ESC_DIM);
  printf("Thinking...");
  esc(ESC_RESET);
  printf("\n");
  ui->lines_drawn = 1;
  fflush(stdout);
}

static bool render_tools(UIState *ui, int frame) {
  erase_dynamic(ui);

  int lines = 0;
  bool all_done = true;
  int cols = terminal_columns();

  for (int i = 0; i < ui->count; i++) {
    ToolSlot *s = &ui->slots[i];
    printf("  ");

    if (s->state == TOOL_RUNNING) {
      all_done = false;
      esc(ESC_GREEN);
      printf("%s ", SPINNER[frame % SPINNER_COUNT]);
      esc(ESC_RESET);
      int label_cols = cols - 4;
      if (label_cols < 1)
        label_cols = 1;
      print_tool_label(s, label_cols);
      printf("\n");
      lines++;
    } else {
      char elapsed_buf[32];
      int elen = snprintf(elapsed_buf, sizeof(elapsed_buf), "(%.1fs)",
                          ts_diff(s->start_time, s->end_time));
      if (elen < 0 || elen >= (int)sizeof(elapsed_buf))
        elen = (int)sizeof(elapsed_buf) - 1;

      esc(s->state == TOOL_DONE ? ESC_GREEN : ESC_RED);
      printf("%s ", s->state == TOOL_DONE ? "\xe2\x9c\x93" : "\xe2\x9c\x97");
      esc(ESC_RESET);

      int label_cols = cols - 5 - elen;
      if (label_cols < 1)
        label_cols = 1;
      print_tool_label(s, label_cols);

      printf(" ");
      esc(ESC_GRAY);
      printf("%s", elapsed_buf);
      esc(ESC_RESET);
      printf("\n");
      lines++;
      lines += render_result_lines(s->result);
    }
  }

  /* Once every slot is done, the final frame stays in the scrollback. */
  ui->lines_drawn = all_done ? 0 : lines;
  fflush(stdout);
  return all_done;
}

/* ── Render thread state machine ───────────────────── */

typedef enum { RS_IDLE, RS_THINKING, RS_TOOLS } RenderPhase;

static void enter_idle(UIState *ui) {
  ui->idle_ack = true;
  pthread_cond_broadcast(&ui->cond);
  esc(ESC_SHOW_CURSOR);
  fflush(stdout);
}

void *render_thread(void *arg) {
  (void)arg;
  UIState *ui = &g_ui;
  RenderPhase phase = RS_IDLE;
  int frame = 0;

  while (1) {
    pthread_mutex_lock(&ui->mutex);

    if (phase == RS_IDLE) {
      while (ui->head == ui->tail)
        pthread_cond_wait(&ui->cond, &ui->mutex);
    } else {
      struct timespec ts;
      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 80000000;
      if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
      }
      pthread_cond_timedwait(&ui->cond, &ui->mutex, &ts);
    }

    bool shutdown = false;
    while (ui->head != ui->tail) {
      UIEvent ev = ui->events[ui->head];
      ui->head = (ui->head + 1) % EVENT_QUEUE_SIZE;

      switch (ev.type) {
      case UI_EV_THINKING:
        if (phase == RS_IDLE) {
          esc(ESC_HIDE_CURSOR);
          fflush(stdout);
        }
        phase = RS_THINKING;
        break;
      case UI_EV_TOOLS_START:
        erase_dynamic(ui);
        phase = RS_TOOLS;
        break;
      case UI_EV_TOOL_DONE:
        break;
      case UI_EV_IDLE:
        /* If tools were running, commit the final frame first:
         * render_tools with all slots done sets lines_drawn=0 so the
         * frame sticks in scrollback; erase_dynamic then becomes a
         * no-op. Without this, ui_idle() arriving before any timed
         * render tick would erase tool output entirely. */
        if (phase == RS_TOOLS)
          render_tools(ui, frame);
        erase_dynamic(ui);
        phase = RS_IDLE;
        frame = 0;
        enter_idle(ui);
        break;
      case UI_EV_SHUTDOWN:
        shutdown = true;
        break;
      }
    }

    if (shutdown) {
      pthread_mutex_unlock(&ui->mutex);
      erase_dynamic(ui);
      esc(ESC_SHOW_CURSOR);
      fflush(stdout);
      break;
    }

    switch (phase) {
    case RS_IDLE:
      break;
    case RS_THINKING:
      render_thinking(ui, frame++);
      break;
    case RS_TOOLS:
      if (render_tools(ui, frame++)) {
        phase = RS_IDLE;
        frame = 0;
        enter_idle(ui);
      }
      break;
    }

    pthread_mutex_unlock(&ui->mutex);
  }

  return NULL;
}
