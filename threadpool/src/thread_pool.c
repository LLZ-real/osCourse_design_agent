/**
 * thread_pool.c - Student implementation scaffold
 *
 * The public API and queue helpers are fixed. You still need to:
 * - define struct thread_pool
 * - FIFO task-queue operations
 * - choose the shared state and wake-up protocol your pool needs
 * - pool initialization and rollback on failure
 * - worker synchronization and lifecycle
 * - wait / destroy semantics
 *
 * You may add private static helpers in this file.
 */

#include <pthread.h>
#include <stdlib.h>

#include "task_queue.h"

struct thread_pool {
    pthread_t *threads;
    int worker_count;
    task_queue tasks;
    pthread_mutex_t tasks_mutex;
    pthread_cond_t tasks_cond;  
    pthread_cond_t idle_cond;
    int shutdown; 
    int idle_workers;
    int destroying;
    /*
     * TODO: add the shared state your design needs.
     *
     * A workable design must let submit(), worker threads, wait(), and destroy()
     * agree on:
     * - whether queued work exists,
     * - whether work is currently executing,
     * - whether shutdown has started,
     * - how sleepers are woken when those facts change.
     */
};

void task_queue_init(task_queue *queue) {
    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;
}

/* TODO A: append one task at the tail while preserving head/tail/count. */
void task_queue_push(task_queue *queue, task *new_task) {
    new_task->next = NULL;
    if(queue->tail == NULL) {
        queue->head = new_task;
    } else {
        queue->tail->next = new_task;
    }
    queue->tail = new_task;
    queue->count++;
}

/* TODO A: pop one task from the head and repair tail when the queue becomes empty. */
task *task_queue_pop(task_queue *queue) {
    task *result = queue->head;
    if (result != NULL) {
        queue->head = result->next;
        if (queue->head == NULL) {
            queue->tail = NULL;
        }
        queue->count--;
    }
    return result;
}

void task_queue_clear(task_queue *queue) {
    task *next_task;

    while ((next_task = task_queue_pop(queue)) != NULL) {
        free(next_task);
    }

    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0;
}

/*
 * TODO B: implement the worker loop.
 *
 * Required behavior:
 * - wait while no task is available
 * - exit cleanly after shutdown
 * - do not hold the synchronization lock that protects shared state
 *   while running the task
 * - after shutdown starts, do not begin any new queued task
 * - maintain shared state so wait/destroy semantics work
 */
void *thread_pool_worker_main(void *pool_arg) {
    while (1) {
        pthread_mutex_lock(&((struct thread_pool *)pool_arg)->tasks_mutex);
        while (((struct thread_pool *)pool_arg)->tasks.count == 0 && !((struct thread_pool *)pool_arg)->shutdown && !((struct thread_pool *)pool_arg)->destroying) {
            pthread_cond_wait(&((struct thread_pool *)pool_arg)->tasks_cond, &((struct thread_pool *)pool_arg)->tasks_mutex);
        }
        if (((struct thread_pool *)pool_arg)->shutdown) {
            pthread_mutex_unlock(&((struct thread_pool *)pool_arg)->tasks_mutex);
            break;
        }
        if (((struct thread_pool *)pool_arg)->destroying) {
            pthread_mutex_unlock(&((struct thread_pool *)pool_arg)->tasks_mutex);
            break;
        }
        task *current_task = task_queue_pop(&((struct thread_pool *)pool_arg)->tasks);
        ((struct thread_pool *)pool_arg)->idle_workers--;

        if (current_task != NULL) {
            pthread_mutex_unlock(&((struct thread_pool *)pool_arg)->tasks_mutex);
            current_task->task_fn(current_task->task_arg);
            pthread_mutex_lock(&((struct thread_pool *)pool_arg)->tasks_mutex);
            ((struct thread_pool *)pool_arg)->idle_workers++;
            pthread_cond_signal(&((struct thread_pool *)pool_arg)->idle_cond);
            pthread_mutex_unlock(&((struct thread_pool *)pool_arg)->tasks_mutex);

            free(current_task);
        }else{
            pthread_mutex_unlock(&((struct thread_pool *)pool_arg)->tasks_mutex);
            break; 
        }
    }
    return NULL;
}

/*
 * TODO B: create the thread pool.
 *
 * You need:
 * - argument validation
 * - pool / thread-array allocation
 * - queue initialization
 * - initialization of the synchronization state your design needs
 * - worker creation
 * - rollback if any step fails after earlier steps succeeded
 */
thread_pool *thread_pool_create(int worker_count) {
    if(worker_count <= 0) {
        return NULL;
    }
    thread_pool *pool = calloc(1, sizeof(thread_pool));
    if (pool == NULL) {
        return NULL;
    }
    pool->threads = calloc(worker_count, sizeof(pthread_t));
    if (pool->threads == NULL) {
        goto cleanup_pool;
    }
    pool->worker_count = worker_count;
    pool->shutdown = 0;
    pool->destroying = 0;
    pool->idle_workers = worker_count;
    task_queue_init(&pool->tasks);
    pthread_mutex_init(&pool->tasks_mutex, NULL);
    pthread_cond_init(&pool->tasks_cond, NULL);
    pthread_cond_init(&pool->idle_cond, NULL);
    int created=0;
    for (int i = 0; i < worker_count; i++) {
        if (pthread_create(&pool->threads[i], NULL, thread_pool_worker_main, pool) != 0) {
            goto cleanup_threads;
        }
        created++;
    }
    
    return pool;

cleanup_threads:   
    for (int i = 0; i < created; i++) {
        pthread_cancel(pool->threads[i]);
        pthread_join(pool->threads[i], NULL);
    }
    free(pool->threads);
cleanup_pool:
    free(pool);
    return NULL;
}

/*
 * TODO B: submit a task safely.
 *
 * Required behavior:
 * - reject invalid input
 * - reject a pool that is already shutting down
 * - wake a waiting worker after a successful push
 * - submission may happen from inside a running worker task
 */
int thread_pool_submit(thread_pool *pool, thread_pool_task_fn task_fn, void *task_arg) {
    pthread_mutex_lock(&pool->tasks_mutex);
    if (pool == NULL || task_fn == NULL) {
        pthread_mutex_unlock(&pool->tasks_mutex);
        return -1;
    }
    task *new_task = malloc(sizeof(task));
    if (new_task == NULL) {
        pthread_mutex_unlock(&pool->tasks_mutex);
        return -1;
    }
    
    if(pool->shutdown || pool->destroying) {
        free(new_task);
        pthread_mutex_unlock(&pool->tasks_mutex);
        return -1;
    }
    new_task->task_fn = task_fn;
    new_task->task_arg = task_arg;
    new_task->next = NULL;
    task_queue_push(&pool->tasks, new_task);
    pthread_cond_signal(&pool->tasks_cond);
    pthread_mutex_unlock(&pool->tasks_mutex);

    return 0;
}

/*
 * TODO C: block until the pool is idle.
 *
 * Idle means:
 * - the queue is empty
 * - no worker is still executing a task
 */
void thread_pool_wait(thread_pool *pool) { 
    if (pool == NULL) {
        return;
    }
    pthread_mutex_lock(&pool->tasks_mutex);
    while (pool->tasks.count > 0 || pool->idle_workers < pool->worker_count) {
        pthread_cond_wait(&pool->idle_cond, &pool->tasks_mutex); 
    }
    pthread_mutex_unlock(&pool->tasks_mutex);
}

/*
 * TODO D: destroy the pool.
 *
 * Required behavior:
 * - let already-started tasks finish
 * - drop not-yet-started tasks
 * - free resources after all workers exit
 */
void thread_pool_destroy(thread_pool *pool) {
    if (pool == NULL) {
        return;
    }

    /* Keep an explicit reference so the starter still builds with -Werror. */
    pthread_mutex_lock(&pool->tasks_mutex);
    pool->destroying = 1;
    pthread_cond_broadcast(&pool->tasks_cond);
    pthread_mutex_unlock(&pool->tasks_mutex);
    for (int i = 0; i < pool->worker_count; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    task_queue_clear(&pool->tasks);
    pthread_mutex_destroy(&pool->tasks_mutex);
    pthread_cond_destroy(&pool->tasks_cond);
    pthread_cond_destroy(&pool->idle_cond);
    free(pool->threads);
    free(pool);
}
