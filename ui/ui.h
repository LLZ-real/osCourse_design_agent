#ifndef UI_H
#define UI_H

#include <stdbool.h>

/*
 * Terminal UI contract.
 *
 * A single render thread owns all stdout redraws for the dynamic region
 * (thinking spinner, running tools). Other threads push events into an
 * internal queue through the functions below; they never touch stdout for
 * dynamic content themselves. The main thread may print final text (LLM
 * responses) only after ui_idle() returns.
 *
 * Lifetime: call ui_init() once at startup. ui_start() spawns the render
 * thread; ui_stop() tears it down.
 */

/* ── Static text output (main thread only) ────────── */

void ui_init(void);
void ui_banner(void);
void ui_prompt(void);
void ui_error(const char *msg);

/* ── Render thread lifecycle ──────────────────────── */

void ui_start(void);
void ui_stop(void);

/* ── Render events ────────────────────────────────── */

typedef struct {
  const char *name;
  const char *args_display; /* may be NULL */
} ToolCallView;

/* Begin a "thinking" indicator. Callable from the main thread. */
void ui_begin_thinking(void);

/* Begin rendering a set of running tools. The UI copies strings internally. */
void ui_begin_tools(int count, const ToolCallView *calls);

/*
 * Mark tool index as finished. Safe to call from any thread. The UI copies
 * the result string internally; the caller retains ownership of its buffer.
 */
void ui_tool_done(int index, bool ok, const char *result);

/*
 * Barrier: block until the render thread has fully drained the dynamic
 * region and released stdout. Call before printing final text from the
 * main thread.
 */
void ui_idle(void);

#endif /* UI_H */
