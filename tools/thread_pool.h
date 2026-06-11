/**
 * thread_pool.h — Fixed-capacity thread pool for the agent executor.
 *
 * Public API: create, submit, wait, destroy.
 * Thread-safe: submit and wait can be called from any thread.
 * destroy must not race with other pool calls.
 */
#ifndef TOOLS_THREAD_POOL_H
#define TOOLS_THREAD_POOL_H

typedef struct thread_pool thread_pool;
typedef void (*thread_pool_task_fn)(void *task_arg);

/* Create a pool with worker_count worker threads. Returns NULL on failure. */
thread_pool *thread_pool_create(int worker_count);

/* Submit one task. Returns 0 on success, -1 on failure. Thread-safe. */
int thread_pool_submit(thread_pool *pool, thread_pool_task_fn fn, void *arg);

/* Block until queue is empty AND no worker is executing a task. */
void thread_pool_wait(thread_pool *pool);

/* Destroy the pool. Finished tasks complete; queued tasks are discarded. */
void thread_pool_destroy(thread_pool *pool);

#endif
