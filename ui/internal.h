#ifndef UI_INTERNAL_H
#define UI_INTERNAL_H

#include "ui/ui.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

/* ── ANSI helpers ──────────────────────────────────── */

extern bool g_color;

#define ESC_RESET "\033[0m"
#define ESC_BOLD "\033[1m"
#define ESC_DIM "\033[2m"
#define ESC_RED "\033[31m"
#define ESC_GREEN "\033[32m"
#define ESC_CYAN "\033[36m"
#define ESC_GRAY "\033[90m"

#define ESC_HIDE_CURSOR "\033[?25l"
#define ESC_SHOW_CURSOR "\033[?25h"

static inline void esc(const char *code) {
  if (g_color)
    fputs(code, stdout);
}

/* ── Render state (private; shared between ui.c and render.c) ─── */

#define MAX_RENDER_SLOTS 32
#define EVENT_QUEUE_SIZE 64

typedef enum { TOOL_RUNNING, TOOL_DONE, TOOL_FAILED } ToolState;

typedef struct {
  char *name;         /* owned */
  char *args_display; /* owned, may be NULL */
  char *result;       /* owned, may be NULL */
  ToolState state;
  struct timespec start_time;
  struct timespec end_time;
} ToolSlot;

typedef enum {
  UI_EV_THINKING,
  UI_EV_TOOLS_START,
  UI_EV_TOOL_DONE,
  UI_EV_IDLE,
  UI_EV_SHUTDOWN,
} UIEventType;

typedef struct {
  UIEventType type;
  int slot;
} UIEvent;

typedef struct {
  ToolSlot slots[MAX_RENDER_SLOTS];
  int count;

  UIEvent events[EVENT_QUEUE_SIZE];
  int head;
  int tail;
  pthread_mutex_t mutex;
  pthread_cond_t cond;

  pthread_t render_tid;
  bool active;
  int lines_drawn;
  bool idle_ack;
} UIState;

extern UIState g_ui;

void *render_thread(void *arg);

#endif /* UI_INTERNAL_H */
