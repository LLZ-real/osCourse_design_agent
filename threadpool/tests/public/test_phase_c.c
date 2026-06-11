#include <pthread.h>
#include <stdio.h>

#include "support.h"
#include "thread_pool.h"

static int passed = 0;
static int total = 0;

static counter_t counter;
static gate_t gate;

typedef struct {
    thread_pool *pool;
    counter_t entered;
    counter_t done;
} wait_thread_arg;

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

static void gated_task(void *arg) {
    gate_t *local = arg;

    pthread_mutex_lock(&local->lock);
    local->started++;
    pthread_cond_broadcast(&local->changed);
    pthread_mutex_unlock(&local->lock);

    gate_blocking_section(local);
}

static void *wait_thread(void *arg) {
    wait_thread_arg *wait_arg = arg;
    counter_inc(&wait_arg->entered);
    thread_pool_wait(wait_arg->pool);
    counter_inc(&wait_arg->done);
    return NULL;
}

int main(void) {
    printf("=== Phase C: thread_pool_wait ===\n\n");
    counter_init(&counter);
    gate_init(&gate);

    {
        thread_pool *pool = thread_pool_create(4);
        check("wait on empty pool returns", pool != NULL);
        thread_pool_wait(pool);
        thread_pool_destroy(pool);
    }

    {
        thread_pool *pool = thread_pool_create(4);
        int ok = (pool != NULL);

        for (int i = 0; i < 100 && ok; i++) {
            ok = (thread_pool_submit(pool, increment_task, &counter) == 0);
        }

        thread_pool_wait(pool);
        check("wait returns after 100 tasks finish", ok && counter_get(&counter) == 100);
        thread_pool_destroy(pool);
        counter_reset(&counter);
    }

    {
        thread_pool *pool = thread_pool_create(2);
        int ok = (pool != NULL);

        for (int i = 0; i < 40 && ok; i++) {
            ok = (thread_pool_submit(pool, increment_task, &counter) == 0);
        }
        thread_pool_wait(pool);

        for (int i = 0; i < 40 && ok; i++) {
            ok = (thread_pool_submit(pool, increment_task, &counter) == 0);
        }
        thread_pool_wait(pool);

        check("wait-submit-wait works across batches", ok && counter_get(&counter) == 80);
        thread_pool_destroy(pool);
        counter_reset(&counter);
    }

    {
        thread_pool *pool = thread_pool_create(2);
        pthread_t waiter;
        wait_thread_arg wait_arg;
        int ok = (pool != NULL);

        gate_reset(&gate);
        counter_init(&wait_arg.entered);
        counter_init(&wait_arg.done);
        wait_arg.pool = pool;

        ok = ok && (thread_pool_submit(pool, gated_task, &gate) == 0);
        ok = ok && (thread_pool_submit(pool, gated_task, &gate) == 0);
        ok = ok && gate_wait_started(&gate, 2, 1000);

        ok = ok && (pthread_create(&waiter, NULL, wait_thread, &wait_arg) == 0);
        ok = ok && counter_wait_at_least(&wait_arg.entered, 1, 1000);

        ok = ok && (counter_get(&wait_arg.done) == 0);

        gate_release_all(&gate);
        ok = ok && (pthread_join(waiter, NULL) == 0);

        ok = ok && (counter_get(&wait_arg.done) == 1);
        ok = ok && gate_wait_finished(&gate, 2, 1000);
        check("wait does not return while workers are still running", ok);

        counter_destroy(&wait_arg.entered);
        counter_destroy(&wait_arg.done);
        thread_pool_destroy(pool);
    }

    gate_destroy(&gate);
    counter_destroy(&counter);

    printf("\n=== Result: %d/%d passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
