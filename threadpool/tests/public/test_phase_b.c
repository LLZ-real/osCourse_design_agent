#include <pthread.h>
#include <stdio.h>

#include "support.h"
#include "thread_pool.h"

static int passed = 0;
static int total = 0;

static counter_t counter;
static thread_pool *reentrant_pool;

static void check(const char *name, int ok) {
    total++;
    if (ok) {
        passed++;
        printf("  [PASS] %s\n", name);
    } else {
        printf("  [FAIL] %s\n", name);
    }
}

static void increment_task(void *arg) {
    counter_t *local = arg;
    counter_inc(local);
}

static void slow_increment_task(void *arg) {
    counter_t *local = arg;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 10 * 1000 * 1000L};
    nanosleep(&ts, NULL);
    counter_inc(local);
}

static void reentrant_chain_task(void *arg) {
    int remaining = (int)(long)arg;
    counter_inc(&counter);
    if (remaining > 0) {
        thread_pool_submit(reentrant_pool, reentrant_chain_task, (void *)(long)(remaining - 1));
    }
}

int main(void) {
    printf("=== Phase B: submit + workers ===\n\n");
    counter_init(&counter);

    {
        thread_pool *pool = thread_pool_create(2);
        int ok;

        ok = (pool != NULL);
        ok = ok && (thread_pool_submit(pool, increment_task, &counter) == 0);
        ok = ok && counter_wait_at_least(&counter, 1, 1000);
        check("single task executes", ok && counter_get(&counter) == 1);
        thread_pool_destroy(pool);
        counter_reset(&counter);
    }

    {
        thread_pool *pool = thread_pool_create(4);
        int ok = (pool != NULL);

        for (int i = 0; i < 100 && ok; i++) {
            ok = (thread_pool_submit(pool, increment_task, &counter) == 0);
        }

        ok = ok && counter_wait_at_least(&counter, 100, 3000);
        check("100 tasks all execute", ok && counter_get(&counter) == 100);
        thread_pool_destroy(pool);
        counter_reset(&counter);
    }

    {
        thread_pool *pool = thread_pool_create(2);
        int ok = (pool != NULL);

        for (int i = 0; i < 8 && ok; i++) {
            ok = (thread_pool_submit(pool, slow_increment_task, &counter) == 0);
        }

        ok = ok && counter_wait_at_least(&counter, 8, 3000);
        check("workers keep draining the task queue", ok && counter_get(&counter) == 8);
        thread_pool_destroy(pool);
        counter_reset(&counter);
    }

    {
        reentrant_pool = thread_pool_create(4);
        int ok = (reentrant_pool != NULL);

        ok = ok && (thread_pool_submit(reentrant_pool, reentrant_chain_task, (void *)9) == 0);
        ok = ok && counter_wait_at_least(&counter, 10, 3000);
        check("re-entrant submit works", ok && counter_get(&counter) == 10);
        thread_pool_destroy(reentrant_pool);
        counter_reset(&counter);
    }

    {
        thread_pool *pool = thread_pool_create(1);
        int ok = (pool != NULL);

        ok = ok && (thread_pool_submit(pool, NULL, NULL) == -1);
        check("NULL task function is rejected", ok);
        thread_pool_destroy(pool);
    }

    counter_destroy(&counter);
    printf("\n=== Result: %d/%d passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
