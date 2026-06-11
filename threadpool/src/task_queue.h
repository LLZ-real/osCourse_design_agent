/**
 * task_queue.h - Fixed queue layer for Lab 3
 *
 * This header exists for exactly two reasons:
 * 1. give thread_pool.c a shared task/queue scaffold,
 * 2. support Phase A white-box tests of FIFO invariants.
 *
 * It does not define struct thread_pool.
 * Students should treat this file as read-only.
 */

#ifndef TASK_QUEUE_H
#define TASK_QUEUE_H

#include "thread_pool.h"

typedef struct task {
    struct task *next;
    thread_pool_task_fn task_fn;
    void *task_arg;
} task;

typedef struct task_queue {
    task *head;
    task *tail;
    int count;
} task_queue;

void task_queue_init(task_queue *queue);
void task_queue_push(task_queue *queue, task *new_task);
task *task_queue_pop(task_queue *queue);
void task_queue_clear(task_queue *queue);

#endif
