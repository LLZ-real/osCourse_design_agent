CC      ?= cc
CFLAGS  := -D_POSIX_C_SOURCE=200809L -std=c11 -Wall -Wextra -Werror -pedantic -g -pthread -Isrc -Itests

ASAN_FLAGS := -fsanitize=address,undefined -fno-omit-frame-pointer
TSAN_FLAGS := -fsanitize=thread -fno-omit-frame-pointer
TIMEOUT    ?= 5
TSAN_TIMEOUT ?= 20

POOL_SRC  := src/thread_pool.c
POOL_HDRS := src/thread_pool.h src/task_queue.h
TESTS     := test_phase_a test_phase_b test_phase_c test_phase_d

.PHONY: all test test-a test-b test-c test-d asan test-asan tsan clean demo \

all: $(TESTS) demo

pool.o: $(POOL_SRC) $(POOL_HDRS)
	$(CC) $(CFLAGS) -c $(POOL_SRC) -o $@

test_phase_a: tests/public/test_phase_a.c tests/support.h $(POOL_HDRS) pool.o
	$(CC) $(CFLAGS) $< pool.o -o $@

test_phase_b: tests/public/test_phase_b.c tests/support.h $(POOL_HDRS) pool.o
	$(CC) $(CFLAGS) $< pool.o -o $@

test_phase_c: tests/public/test_phase_c.c tests/support.h $(POOL_HDRS) pool.o
	$(CC) $(CFLAGS) $< pool.o -o $@

test_phase_d: tests/public/test_phase_d.c tests/support.h $(POOL_HDRS) pool.o
	$(CC) $(CFLAGS) $< pool.o -o $@

demo: src/demo_attention.c pool.o
	$(CC) $(CFLAGS) $< pool.o -o $@ -lm

test: $(TESTS)
	python3 tests/run_tests.py --timeout $(TIMEOUT)

test-a test-b test-c test-d: $(TESTS)
	python3 tests/run_tests.py --timeout $(TIMEOUT) --phase $(@:test-%=%)

asan:
	$(MAKE) clean all CFLAGS="$(CFLAGS) $(ASAN_FLAGS)"
	python3 tests/run_tests.py --timeout $(TIMEOUT)

test-asan: asan

tsan:
	$(MAKE) clean all CFLAGS="$(CFLAGS) $(TSAN_FLAGS)"
	python3 tests/run_tests.py --timeout $(TSAN_TIMEOUT)

clean:
	rm -f $(TESTS) demo pool.o pool-ref.o test_hidden
	find . -name '*.dSYM' -type d -exec rm -rf {} + 2>/dev/null || true
