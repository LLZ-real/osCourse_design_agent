#ifndef TEST_SUPPORT_H
#define TEST_SUPPORT_H

#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int value;
} counter_t;

static inline struct timespec timeout_from_now_ms(int timeout_ms) {
    struct timespec ts;
    long nsec;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    nsec = ts.tv_nsec + (long)(timeout_ms % 1000) * 1000000L;
    ts.tv_sec += nsec / 1000000000L;
    ts.tv_nsec = nsec % 1000000000L;
    return ts;
}

static inline void counter_init(counter_t *counter) {
    pthread_mutex_init(&counter->lock, NULL);
    pthread_cond_init(&counter->changed, NULL);
    counter->value = 0;
}

static inline void counter_destroy(counter_t *counter) {
    pthread_cond_destroy(&counter->changed);
    pthread_mutex_destroy(&counter->lock);
}

static inline void counter_reset(counter_t *counter) {
    pthread_mutex_lock(&counter->lock);
    counter->value = 0;
    pthread_cond_broadcast(&counter->changed);
    pthread_mutex_unlock(&counter->lock);
}

static inline void counter_inc(counter_t *counter) {
    pthread_mutex_lock(&counter->lock);
    counter->value++;
    pthread_cond_broadcast(&counter->changed);
    pthread_mutex_unlock(&counter->lock);
}

static inline int counter_get(counter_t *counter) {
    int value;

    pthread_mutex_lock(&counter->lock);
    value = counter->value;
    pthread_mutex_unlock(&counter->lock);
    return value;
}

static inline int counter_wait_at_least(counter_t *counter, int expected, int timeout_ms) {
    int ok;
    int rc = 0;
    struct timespec deadline = timeout_from_now_ms(timeout_ms);

    pthread_mutex_lock(&counter->lock);
    while (counter->value < expected && rc != ETIMEDOUT) {
        rc = pthread_cond_timedwait(&counter->changed, &counter->lock, &deadline);
    }
    ok = counter->value >= expected;
    pthread_mutex_unlock(&counter->lock);
    return ok;
}

typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    int started;
    int finished;
    int released;
} gate_t;

static inline void gate_init(gate_t *gate) {
    pthread_mutex_init(&gate->lock, NULL);
    pthread_cond_init(&gate->changed, NULL);
    gate->started = 0;
    gate->finished = 0;
    gate->released = 0;
}

static inline void gate_destroy(gate_t *gate) {
    pthread_cond_destroy(&gate->changed);
    pthread_mutex_destroy(&gate->lock);
}

static inline void gate_reset(gate_t *gate) {
    pthread_mutex_lock(&gate->lock);
    gate->started = 0;
    gate->finished = 0;
    gate->released = 0;
    pthread_cond_broadcast(&gate->changed);
    pthread_mutex_unlock(&gate->lock);
}

static inline int gate_started(gate_t *gate) {
    int value;

    pthread_mutex_lock(&gate->lock);
    value = gate->started;
    pthread_mutex_unlock(&gate->lock);
    return value;
}

static inline int gate_finished(gate_t *gate) {
    int value;

    pthread_mutex_lock(&gate->lock);
    value = gate->finished;
    pthread_mutex_unlock(&gate->lock);
    return value;
}

static inline void gate_snapshot(gate_t *gate, int *started, int *finished) {
    pthread_mutex_lock(&gate->lock);
    *started = gate->started;
    *finished = gate->finished;
    pthread_mutex_unlock(&gate->lock);
}

static inline int gate_wait_started(gate_t *gate, int expected, int timeout_ms) {
    int ok;
    int rc = 0;
    struct timespec deadline = timeout_from_now_ms(timeout_ms);

    pthread_mutex_lock(&gate->lock);
    while (gate->started < expected && rc != ETIMEDOUT) {
        rc = pthread_cond_timedwait(&gate->changed, &gate->lock, &deadline);
    }
    ok = gate->started >= expected;
    pthread_mutex_unlock(&gate->lock);
    return ok;
}

static inline int gate_wait_finished(gate_t *gate, int expected, int timeout_ms) {
    int ok;
    int rc = 0;
    struct timespec deadline = timeout_from_now_ms(timeout_ms);

    pthread_mutex_lock(&gate->lock);
    while (gate->finished < expected && rc != ETIMEDOUT) {
        rc = pthread_cond_timedwait(&gate->changed, &gate->lock, &deadline);
    }
    ok = gate->finished >= expected;
    pthread_mutex_unlock(&gate->lock);
    return ok;
}

static inline void gate_release_all(gate_t *gate) {
    pthread_mutex_lock(&gate->lock);
    gate->released = 1;
    pthread_cond_broadcast(&gate->changed);
    pthread_mutex_unlock(&gate->lock);
}

static inline void gate_blocking_section(gate_t *gate) {
    pthread_mutex_lock(&gate->lock);
    while (!gate->released) {
        pthread_cond_wait(&gate->changed, &gate->lock);
    }
    gate->finished++;
    pthread_cond_broadcast(&gate->changed);
    pthread_mutex_unlock(&gate->lock);
}

#endif
