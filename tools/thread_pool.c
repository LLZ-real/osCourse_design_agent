/**
 * thread_pool.c — Thread pool implementation for the agent executor.
 *
 * Based on the Lab 3 thread pool. Internal task queue uses a singly-linked
 * FIFO list. Workers block on a condition variable when no work is available.
 * The pool tracks idle workers so thread_pool_wait can distinguish "queue
 * empty" from "queue empty AND no task is executing."
 */
#include "tools/thread_pool.h"

#include <pthread.h>
#include <stdlib.h>

/* ── Internal task queue ─────────────────────────────────────────────── */

typedef struct task {
    struct task *next;
    thread_pool_task_fn task_fn;
    void *task_arg;
} task;

typedef struct {
    task *head;
    task *tail;
    int count;
} task_queue;

static void tq_init(task_queue *q) {
    q->head = NULL;
    q->tail = NULL;
    q->count = 0;
}

static void tq_push(task_queue *q, task *t) {
    t->next = NULL;
    if (q->tail)
        q->tail->next = t;
    else
        q->head = t;
    q->tail = t;
    q->count++;
}

static task *tq_pop(task_queue *q) {
    task *t = q->head;
    if (!t)
        return NULL;
    q->head = t->next;
    if (!q->head)
        q->tail = NULL;
    q->count--;
    return t;
}

static void tq_clear(task_queue *q) {
    task *t;
    while ((t = tq_pop(q)))
        free(t);
}

/* ── Thread pool ─────────────────────────────────────────────────────── */

struct thread_pool {
    pthread_t *threads;
    int worker_count;

    task_queue tasks;
    int idle_workers;   /* workers not currently running a task */
    int shutdown;       /* polite shutdown requested */
    int destroying;     /* immediate shutdown requested */

    pthread_mutex_t lock;
    pthread_cond_t wake_cond;  /* signaled when a task is queued */
    pthread_cond_t idle_cond;  /* signaled when idle_workers == worker_count */
};

static void *worker_main(void *arg) {
    thread_pool *pool = arg;

    for (;;) {
        pthread_mutex_lock(&pool->lock);

        while (pool->tasks.count == 0 && !pool->shutdown && !pool->destroying)
            pthread_cond_wait(&pool->wake_cond, &pool->lock);

        if (pool->shutdown || pool->destroying) {
            pthread_mutex_unlock(&pool->lock);
            break;
        }

        task *t = tq_pop(&pool->tasks);
        /* t is never NULL here: count > 0 guarantees a task, but in theory
           a spurious wakeup could land between count check and pop. */
        if (!t) {
            pthread_mutex_unlock(&pool->lock);
            continue;
        }

        pool->idle_workers--;
        pthread_mutex_unlock(&pool->lock);

        t->task_fn(t->task_arg);

        pthread_mutex_lock(&pool->lock);
        pool->idle_workers++;
        pthread_cond_signal(&pool->idle_cond);
        pthread_mutex_unlock(&pool->lock);

        free(t);
    }

    return NULL;
}

thread_pool *thread_pool_create(int worker_count) {
    if (worker_count <= 0)
        return NULL;

    thread_pool *pool = calloc(1, sizeof(*pool));
    if (!pool)
        return NULL;

    pool->threads = calloc((size_t)worker_count, sizeof(pthread_t));
    if (!pool->threads) {
        free(pool);
        return NULL;
    }

    pool->worker_count = worker_count;
    pool->idle_workers = worker_count;
    tq_init(&pool->tasks);

    pthread_mutex_init(&pool->lock, NULL);
    pthread_cond_init(&pool->wake_cond, NULL);
    pthread_cond_init(&pool->idle_cond, NULL);

    int created = 0;
    for (int i = 0; i < worker_count; i++) {
        if (pthread_create(&pool->threads[i], NULL, worker_main, pool) != 0)
            goto fail;
        created++;
    }

    return pool;

fail:
    for (int i = 0; i < created; i++) {
        pthread_cancel(pool->threads[i]);
        pthread_join(pool->threads[i], NULL);
    }
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->wake_cond);
    pthread_cond_destroy(&pool->idle_cond);
    free(pool->threads);
    free(pool);
    return NULL;
}

int thread_pool_submit(thread_pool *pool, thread_pool_task_fn fn, void *arg) {
    if (!pool || !fn)
        return -1;

    task *t = malloc(sizeof(*t));
    if (!t)
        return -1;

    t->task_fn = fn;
    t->task_arg = arg;

    pthread_mutex_lock(&pool->lock);

    if (pool->shutdown || pool->destroying) {
        pthread_mutex_unlock(&pool->lock);
        free(t);
        return -1;
    }

    tq_push(&pool->tasks, t);
    pthread_cond_signal(&pool->wake_cond);
    pthread_mutex_unlock(&pool->lock);

    return 0;
}

void thread_pool_wait(thread_pool *pool) {
    if (!pool)
        return;

    pthread_mutex_lock(&pool->lock);
    while (pool->tasks.count > 0 || pool->idle_workers < pool->worker_count)
        pthread_cond_wait(&pool->idle_cond, &pool->lock);
    pthread_mutex_unlock(&pool->lock);
}

void thread_pool_destroy(thread_pool *pool) {
    if (!pool)
        return;

    pthread_mutex_lock(&pool->lock);
    pool->destroying = 1;
    pthread_cond_broadcast(&pool->wake_cond);
    pthread_mutex_unlock(&pool->lock);

    for (int i = 0; i < pool->worker_count; i++)
        pthread_join(pool->threads[i], NULL);

    tq_clear(&pool->tasks);
    pthread_mutex_destroy(&pool->lock);
    pthread_cond_destroy(&pool->wake_cond);
    pthread_cond_destroy(&pool->idle_cond);
    free(pool->threads);
    free(pool);
}
