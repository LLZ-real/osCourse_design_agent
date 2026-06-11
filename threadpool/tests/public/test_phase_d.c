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
} destroy_thread_arg;

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

static void *destroy_thread_main(void *arg) {
    destroy_thread_arg *destroy_arg = arg;
    counter_inc(&destroy_arg->entered);
    thread_pool_destroy(destroy_arg->pool);
    counter_inc(&destroy_arg->done);
    return NULL;
}

int main(void) {
    printf("=== Phase D: thread_pool_destroy ===\n\n");
    counter_init(&counter);
    gate_init(&gate);

    thread_pool_destroy(NULL);
    check("destroy(NULL) is safe", 1);

    {
        thread_pool *pool = thread_pool_create(4);
        check("create + immediate destroy does not hang", pool != NULL);
        thread_pool_destroy(pool);
    }

    {
        thread_pool *pool = thread_pool_create(4);
        int ok = (pool != NULL);

        for (int i = 0; i < 100 && ok; i++) {
            ok = (thread_pool_submit(pool, increment_task, &counter) == 0);
        }

        thread_pool_wait(pool);
        thread_pool_destroy(pool);
        check("destroy after wait keeps completed tasks", ok && counter_get(&counter) == 100);
        counter_reset(&counter);
    }

    {
        thread_pool *pool = thread_pool_create(2);
        pthread_t destroyer;
        destroy_thread_arg destroy_arg;
        int ok = (pool != NULL);
        int started = 0;
        int finished = 0;

        gate_reset(&gate);
        counter_init(&destroy_arg.entered);
        counter_init(&destroy_arg.done);
        destroy_arg.pool = pool;
        for (int i = 0; i < 6 && ok; i++) {
            ok = (thread_pool_submit(pool, gated_task, &gate) == 0);
        }

        ok = ok && gate_wait_started(&gate, 2, 1000);
        ok = ok && (pthread_create(&destroyer, NULL, destroy_thread_main, &destroy_arg) == 0);
        ok = ok && counter_wait_at_least(&destroy_arg.entered, 1, 1000);
        ok = ok && (counter_get(&destroy_arg.done) == 0);
        gate_release_all(&gate);
        ok = ok && (pthread_join(destroyer, NULL) == 0);

        gate_snapshot(&gate, &started, &finished);
        ok = ok && (started == 2);
        ok = ok && (finished == 2);
        check("destroy keeps in-flight work and discards pending work", ok);
        counter_destroy(&destroy_arg.entered);
        counter_destroy(&destroy_arg.done);
    }

    {
        int ok = 1;

        for (int round = 0; round < 5 && ok; round++) {
            thread_pool *pool = thread_pool_create(2);
            ok = (pool != NULL);
            for (int i = 0; i < 20 && ok; i++) {
                ok = (thread_pool_submit(pool, increment_task, &counter) == 0);
            }
            thread_pool_wait(pool);
            thread_pool_destroy(pool);
            ok = ok && (counter_get(&counter) == 20);
            counter_reset(&counter);
        }

        check("rapid create/destroy cycles remain correct", ok);
    }

    gate_destroy(&gate);
    counter_destroy(&counter);

    printf("\n=== Result: %d/%d passed ===\n", passed, total);
    return (passed == total) ? 0 : 1;
}
