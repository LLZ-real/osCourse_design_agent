/**
 * demo_attention.c — Demo: multi-head self-attention
 *
 * Already provided:
 * - a serial version
 * - a wrapper for one attention head
 * - correctness and timing comparisons
 *
 * You only need to:
 * - submit one task per head
 * - call thread_pool_wait() before reading the output
 */

#include "thread_pool.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SEQ_LEN 128
#define D_MODEL 512
#define N_HEADS 8
#define D_K (D_MODEL / N_HEADS)
#define REPEATS 5

static void matmul(const float *A, const float *B, float *C, int M, int K, int N) {
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < N; j++) {
            float sum = 0.0f;
            for (int k = 0; k < K; k++) {
                sum += A[i * K + k] * B[k * N + j];
            }
            C[i * N + j] = sum;
        }
    }
}

static void transpose(const float *A, float *B, int M, int N) {
    for (int i = 0; i < M; i++) {
        for (int j = 0; j < N; j++) {
            B[j * M + i] = A[i * N + j];
        }
    }
}

static void softmax_rows(float *mat, int rows, int cols) {
    for (int i = 0; i < rows; i++) {
        float *row = &mat[i * cols];
        float max_val = row[0];
        float sum = 0.0f;

        for (int j = 1; j < cols; j++) {
            if (row[j] > max_val) {
                max_val = row[j];
            }
        }

        for (int j = 0; j < cols; j++) {
            row[j] = expf(row[j] - max_val);
            sum += row[j];
        }

        for (int j = 0; j < cols; j++) {
            row[j] /= sum;
        }
    }
}

static void extract_head(const float *full, float *head_buf, int head_idx) {
    for (int i = 0; i < SEQ_LEN; i++) {
        memcpy(&head_buf[i * D_K], &full[i * D_MODEL + head_idx * D_K], D_K * sizeof(float));
    }
}

static void write_head(float *full, const float *head_buf, int head_idx) {
    for (int i = 0; i < SEQ_LEN; i++) {
        memcpy(&full[i * D_MODEL + head_idx * D_K], &head_buf[i * D_K], D_K * sizeof(float));
    }
}

static void attention_single_head(
    const float *Q_h, const float *K_h, const float *V_h, float *out_h, float *scores_buf
) {
    float K_t[D_K * SEQ_LEN];
    float scale = 1.0f / sqrtf((float)D_K);

    transpose(K_h, K_t, SEQ_LEN, D_K);
    matmul(Q_h, K_t, scores_buf, SEQ_LEN, D_K, SEQ_LEN);

    for (int i = 0; i < SEQ_LEN * SEQ_LEN; i++) {
        scores_buf[i] *= scale;
    }

    softmax_rows(scores_buf, SEQ_LEN, SEQ_LEN);
    matmul(scores_buf, V_h, out_h, SEQ_LEN, SEQ_LEN, D_K);
}

static void
attention_multi_head_serial(const float *Q, const float *K, const float *V, float *output) {
    float Q_h[SEQ_LEN * D_K];
    float K_h[SEQ_LEN * D_K];
    float V_h[SEQ_LEN * D_K];
    float out_h[SEQ_LEN * D_K];
    float scores[SEQ_LEN * SEQ_LEN];

    for (int h = 0; h < N_HEADS; h++) {
        extract_head(Q, Q_h, h);
        extract_head(K, K_h, h);
        extract_head(V, V_h, h);
        attention_single_head(Q_h, K_h, V_h, out_h, scores);
        write_head(output, out_h, h);
    }
}

typedef struct {
    const float *Q;
    const float *K;
    const float *V;
    float *output;
    int head_idx;
} head_arg;

static void run_attention_head(void *arg) {
    head_arg *a = arg;
    float Q_h[SEQ_LEN * D_K];
    float K_h[SEQ_LEN * D_K];
    float V_h[SEQ_LEN * D_K];
    float out_h[SEQ_LEN * D_K];
    float scores[SEQ_LEN * SEQ_LEN];

    extract_head(a->Q, Q_h, a->head_idx);
    extract_head(a->K, K_h, a->head_idx);
    extract_head(a->V, V_h, a->head_idx);
    attention_single_head(Q_h, K_h, V_h, out_h, scores);
    write_head(a->output, out_h, a->head_idx);
}

static void attention_multi_head_parallel(
    thread_pool *pool, const float *Q, const float *K, const float *V, float *output
) {
    head_arg args[N_HEADS];
    for (int h = 0; h < N_HEADS; h++) {
        args[h].Q = Q;
        args[h].K = K;
        args[h].V = V;
        args[h].output = output;
        args[h].head_idx = h;
    }

    /*
     * TODO Demo:
     * 1. Call thread_pool_submit(pool, run_attention_head, &args[h]) for
     *    each head.
     * 2. Call thread_pool_wait(pool).
     */
    thread_pool_submit(pool, run_attention_head, &args[0]);
    thread_pool_submit(pool, run_attention_head, &args[1]);
    thread_pool_submit(pool, run_attention_head, &args[2]);
    thread_pool_submit(pool, run_attention_head, &args[3]);
    thread_pool_submit(pool, run_attention_head, &args[4]);
    thread_pool_submit(pool, run_attention_head, &args[5]);
    thread_pool_submit(pool, run_attention_head, &args[6]);
    thread_pool_submit(pool, run_attention_head, &args[7]);
    thread_pool_wait(pool);
    (void)pool;
    (void)run_attention_head;
}

static void rand_init(float *mat, int size) {
    for (int i = 0; i < size; i++) {
        mat[i] = (float)rand() / (float)RAND_MAX - 0.5f;
    }
}

static double get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(void) {
    int mat_size = SEQ_LEN * D_MODEL;
    float *Q;
    float *K;
    float *V;
    float *out_s;
    float *out_p;
    double t0;
    double t_serial;
    double t_parallel;
    float max_diff = 0.0f;
    thread_pool *pool;

    srand(42);

    printf("=== Transformer Self-Attention Demo ===\n");
    printf("  seq_len=%d, d_model=%d, n_heads=%d, d_k=%d\n", SEQ_LEN, D_MODEL, N_HEADS, D_K);
    printf("  repeats=%d\n\n", REPEATS);

    Q = malloc((size_t)mat_size * sizeof(float));
    K = malloc((size_t)mat_size * sizeof(float));
    V = malloc((size_t)mat_size * sizeof(float));
    out_s = malloc((size_t)mat_size * sizeof(float));
    out_p = malloc((size_t)mat_size * sizeof(float));
    if (Q == NULL || K == NULL || V == NULL || out_s == NULL || out_p == NULL) {
        free(Q);
        free(K);
        free(V);
        free(out_s);
        free(out_p);
        return 1;
    }

    rand_init(Q, mat_size);
    rand_init(K, mat_size);
    rand_init(V, mat_size);
    memset(out_s, 0, (size_t)mat_size * sizeof(float));
    memset(out_p, 0, (size_t)mat_size * sizeof(float));

    t0 = get_time();
    for (int r = 0; r < REPEATS; r++) {
        attention_multi_head_serial(Q, K, V, out_s);
    }
    t_serial = (get_time() - t0) / REPEATS;
    printf("Serial:   %.4f s\n", t_serial);

    pool = thread_pool_create(N_HEADS);
    t0 = get_time();
    for (int r = 0; r < REPEATS; r++) {
        attention_multi_head_parallel(pool, Q, K, V, out_p);
    }
    t_parallel = (get_time() - t0) / REPEATS;
    printf("Parallel: %.4f s  (workers=%d)\n", t_parallel, N_HEADS);

    for (int i = 0; i < mat_size; i++) {
        float diff = fabsf(out_s[i] - out_p[i]);
        if (diff > max_diff) {
            max_diff = diff;
        }
    }

    printf("\nMax diff: %.6e  (%s)\n", max_diff, max_diff < 1e-4f ? "PASS" : "FAIL");
    if (max_diff > 1e-4f) {
        printf("  (This is expected if you have not finished the demo yet.)\n");
    }

    if (t_parallel > 1e-9) {
        printf("Speedup:  %.2fx\n", t_serial / t_parallel);
    }

    thread_pool_destroy(pool);
    free(Q);
    free(K);
    free(V);
    free(out_s);
    free(out_p);
    return 0;
}
