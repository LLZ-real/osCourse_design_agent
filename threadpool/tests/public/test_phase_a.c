#include <stdio.h>

#include "task_queue.h"

static int passed = 0;
static int total = 0;

static void check(const char *name, int ok) {
    total++;
    if (ok) {
        passed++;
        printf("  [PASS] %s\n", name);
    } else {
        printf("  [FAIL] %s\n", name);
    }
}

static void dummy(void *arg) { (void)arg; }

int main(void) {
    printf("=== Phase A: task queue invariants ===\n\n");

    {
        task_queue queue;
        task first_task = {.next = NULL, .task_fn = dummy, .task_arg = (void *)1};

        task_queue_init(&queue);
        task_queue_push(&queue, &first_task);
        check("push one: count == 1", queue.count == 1);
        check("push one: head == tail", queue.head == queue.tail);
        check("push one: head points to first task", queue.head == &first_task);
        check("push one: tail->next == NULL", queue.tail != NULL && queue.tail->next == NULL);
    }

    {
        task_queue queue;

        task_queue_init(&queue);
        check("empty queue: head == NULL", queue.head == NULL);
        check("empty queue: tail == NULL", queue.tail == NULL);
        check("empty queue: count == 0", queue.count == 0);
        check("pop empty queue returns NULL", task_queue_pop(&queue) == NULL);
    }

    {
        task_queue queue;
        task first_task = {.next = NULL, .task_fn = dummy, .task_arg = (void *)1};
        task second_task = {.next = NULL, .task_fn = dummy, .task_arg = (void *)2};
        task third_task = {.next = NULL, .task_fn = dummy, .task_arg = (void *)3};

        task_queue_init(&queue);
        task_queue_push(&queue, &first_task);
        task_queue_push(&queue, &second_task);
        task_queue_push(&queue, &third_task);

        check("push three: count == 3", queue.count == 3);
        check("push three: FIFO first", task_queue_pop(&queue) == &first_task);
        check("push three: FIFO second", task_queue_pop(&queue) == &second_task);
        check("push three: FIFO third", task_queue_pop(&queue) == &third_task);
        check("pop all: count == 0", queue.count == 0);
        check("pop all: head == NULL", queue.head == NULL);
        check("pop all: tail == NULL", queue.tail == NULL);
    }

    {
        task_queue queue;
        task first_task = {.next = NULL, .task_fn = dummy, .task_arg = (void *)1};
        task second_task = {.next = NULL, .task_fn = dummy, .task_arg = (void *)2};

        task_queue_init(&queue);
        task_queue_push(&queue, &first_task);
        (void)task_queue_pop(&queue);
        task_queue_push(&queue, &second_task);

        check("push after drain: count == 1", queue.count == 1);
        check("push after drain: head points to second task", queue.head == &second_task);
        check("push after drain: tail points to second task", queue.tail == &second_task);
    }

    {
        task_queue queue;
        task tasks[8];
        int ok = 1;

        task_queue_init(&queue);
        for (int i = 0; i < 8; i++) {
            tasks[i].next = NULL;
            tasks[i].task_fn = dummy;
            tasks[i].task_arg = (void *)(long)i;
            task_queue_push(&queue, &tasks[i]);
        }

        for (int i = 0; i < 8; i++) {
            if (task_queue_pop(&queue) != &tasks[i]) {
                ok = 0;
                break;
            }
        }

        check("many pushes preserve FIFO", ok);
        check("many pushes preserve count", queue.count == 0);
    }

    printf("\n=== Result: %d/%d passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
