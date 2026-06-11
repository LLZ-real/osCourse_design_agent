/**
 * thread_pool.h - Thread Pool public API
 *
 * Students implement thread_pool.c against these public contracts.
 * Queue scaffolding for Phase A lives in task_queue.h.
 * The pool's private layout belongs in thread_pool.c.
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

typedef struct thread_pool thread_pool;
typedef void (*thread_pool_task_fn)(void *task_arg);

/**
 * Create a thread pool with worker_count worker threads.
 *
 * Return a pool handle on success, or NULL on failure.
 * Also return NULL when worker_count <= 0.
 */
thread_pool *thread_pool_create(int worker_count);

/**
 * Submit one task to the pool.
 *
 * Return values:
 *   0   submission succeeded
 *  -1   invalid input, stopped pool, or task allocation failure
 *
 * Thread-safe: multiple callers may submit concurrently, including workers.
 */
int thread_pool_submit(thread_pool *pool, thread_pool_task_fn task_fn, void *task_arg);

/**
 * Block until the queue is empty and no worker is still running a task.
 *
 * Thread-safe: may be called repeatedly.
 */
void thread_pool_wait(thread_pool *pool);

/**
 * Destroy the pool.
 *
 * Required semantics:
 * - let already-running tasks finish
 * - discard queued-but-not-started tasks
 * - accept NULL safely
 *
 * This function should not run concurrently with other thread_pool_* calls.
 */
void thread_pool_destroy(thread_pool *pool);

#endif
