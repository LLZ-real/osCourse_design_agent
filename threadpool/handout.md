# Lab 3: Thread Pool — Manage Concurrent Work

When you ask an AI coding assistant to "read these 5 files", it doesn't read
them one by one. It dispatches all five reads to a thread pool, lets them run in
parallel, collects the results, and sends them back to the LLM in the original
order. The speed improvement is obvious, but the real challenge is invisible:
how do you coordinate multiple threads sharing the same queue without corrupting
data, missing wake-ups, or deadlocking?

In this lab, you will build a thread pool from scratch using POSIX threads,
mutexes, and condition variables. The difficult part is not "making things run
fast". It is designing a small concurrent state machine where `create`,
`submit`, and `wait` all agree on the state of the world — even when they
execute simultaneously from different threads. `destroy` is different: it is a
terminal operation and should not run concurrently with other
`thread_pool_*` calls.

---

## 1. What You Are Building

A thread pool with four public operations:

| Function                            | What it does                 |
| ----------------------------------- | ---------------------------- |
| `thread_pool_create(n)`             | Spawn `n` worker threads     |
| `thread_pool_submit(pool, fn, arg)` | Enqueue a task for execution |
| `thread_pool_wait(pool)`            | Block until all work is done |
| `thread_pool_destroy(pool)`         | Shut down and clean up       |

Workers sit in a loop: wait for a task, execute it, wait again. The pool is the
classic **producer-consumer** pattern: `submit()` produces tasks, workers
consume them.

**You submit only `src/thread_pool.c`.** The public API (`thread_pool.h`) and
queue interface (`task_queue.h`) are fixed. You must define `struct thread_pool`
yourself and implement all the functions.

---

## 2. Before You Start

### 2.1 Read the headers

Before writing code, read `src/thread_pool.h` and `src/task_queue.h`. They
define the public contract your code must satisfy. Pay special attention to the
doc comments — they are your specification.

The `task` struct is a singly-linked list node with a function pointer and an
argument. The `task_queue` is a FIFO queue with head, tail, and count.

> **`void *` — C's generic pointer.** The `task_arg` field is `void *`, which
> means "a pointer to anything". The task function receives it and casts it to
> the correct type. This is how C implements generic callbacks. In C++, you'd use
> templates or `std::function`; in Python, closures capture their context
> naturally; in Rust, you'd use trait objects or generics.

> **Opaque pointers.** The header declares
> `typedef struct thread_pool thread_pool;` without defining the struct.
> Callers use `thread_pool *` without seeing the internals. This is C's version
> of encapsulation — users interact
> with the pool only through the four public functions.

### 2.2 Build and test

```bash
make          # build
make test-a   # Phase A: queue tests
make test-b   # Phase B: worker lifecycle
make test-c   # Phase C: wait semantics
make test-d   # Phase D: destroy semantics
make test     # all phases
make asan     # AddressSanitizer
make test-asan
make tsan     # ThreadSanitizer — ESSENTIAL for this lab
```

### 2.3 ThreadSanitizer

```bash
make tsan     # builds with -fsanitize=thread
```

ThreadSanitizer detects data races — when two threads access the same memory
without synchronization and at least one is a write. If your code "works" but
TSan reports a race, **the race is real**. Do not dismiss it.

> **Why races are dangerous.** A data race means the outcome depends on timing.
> Your program might pass tests 99 times and fail on the 100th. TSan catches
> these before they become intermittent, impossible-to-reproduce bugs.
>
> The equivalent in other ecosystems: Rust's borrow checker prevents data races
> at compile time. Go's race detector (`go run -race`) works similarly to TSan.
> Python's GIL prevents true parallelism (and thus many data races) but also
> limits performance. Now there are also Python implementations without GIL.

---

## 3. Phase A — FIFO Queue

### 3.1 The goal

Implement `task_queue_push()` and `task_queue_pop()` so they maintain a correct
FIFO (first-in, first-out) linked list.

This phase has no threads. It's pure data structure work. Get it right before
adding concurrency.

### 3.2 Linked list invariants

The queue has three invariants that must hold at all times:

1. If `head == NULL` then `tail == NULL` and `count == 0` (empty queue).
2. If `head != NULL` then `tail != NULL` and `count > 0`.
3. Following `next` pointers from `head` visits exactly `count` nodes and ends
   at `tail`.

Every push and pop must preserve all three. The tests verify this with
white-box checks.

The most common bug: when the last element is popped, forgetting to set `tail`
to NULL. This leaves `tail` as a dangling pointer — the next `push` would
corrupt memory.

### 3.3 Verify before moving on

```bash
make test-a
```

All Phase A tests should pass before you touch any threading code.

---

## 4. Phase B — Worker Lifecycle and Submission

### 4.1 The goal

Create worker threads that pull tasks from the queue and execute them.
`submit()` must be thread-safe: multiple threads (including workers themselves)
can submit tasks concurrently.

### 4.2 Designing the shared state

Before writing code, think carefully about what state the pool needs and who
accesses it. This is the critical design step.

The starter code gives you a `struct thread_pool` with `threads`,
`worker_count`, `tasks`, and a placeholder `unused` field. You need to replace
`unused` with the synchronization state your design requires.

Consider these questions:

- **How many mutexes do you need?** A single mutex protecting all shared state
  is the simplest correct design. Could you use more? Yes, but the complexity
  cost is high and the benefit is negligible for this lab.

- **How many condition variables do you need?** Think about the distinct events
  that threads need to wait for. Workers need to sleep when the queue is empty.
  Is there another event that a different caller needs to wait for? (Hint: think
  about what `wait()` needs to know.)

- **What facts must all parties agree on?** The TODO comment in the starter code
  lists them: whether queued work exists, whether work is currently executing,
  whether shutdown has started, and how sleepers are woken when those facts
  change.

> **Keyword to explore:** "producer-consumer pattern with mutex and condition
> variables".

### 4.3 The worker loop

Each worker runs a loop: lock, check for work, sleep if none, dequeue a task,
unlock, execute, repeat. Look up `pthread_cond_wait` — it atomically releases
the mutex and puts the thread to sleep, then reacquires the mutex when woken.

Two critical rules:

1. **Use `while`, not `if`, around `pthread_cond_wait`.** POSIX allows
   **spurious wakeups** — the thread may wake up even when nobody signaled.
   Also, in a multi-worker pool, a signal might wake two workers but only one
   task is available. The woken thread must re-check the condition.

2. **Do not hold the lock while executing the task.** If workers held the lock
   during task execution, only one task could run at a time — the "pool" would
   be serialized. Worse, if a task calls `submit()` (re-entrant submission) and
   `submit()` needs the lock, you get deadlock.

> **Function pointers as callbacks.** The task's `task_fn` field stores the
> address of a function. When the worker calls `task_fn(task_arg)`, it invokes
> whatever function was passed to `submit()`. This is how the pool stays
> generic. Same mechanism as Python's `threading.Thread(target=fn)`.

### 4.4 Creating the pool

The creation function must validate input, allocate resources, initialize
synchronization primitives, and create worker threads. If any step fails after
earlier steps succeeded, you must roll back — free what was allocated, destroy
what was initialized.

Look up the `goto cleanup` pattern — it is the standard C idiom for multi-step
resource acquisition with rollback. The Linux kernel uses it extensively.

> **Cleanup in other languages.** C++ uses RAII (destructors handle
> cleanup), Python uses `try/finally` or `with`, Go uses `defer`, Rust uses
> `Drop`. All solve the same problem — C's `goto` is the manual equivalent.

> **`calloc` vs `malloc`.** `calloc(n, size)` allocates and zeroes memory.
> `malloc(size)` allocates without zeroing. For structs that need initial zeroed
> state, `calloc` is safer — uninitialized memory in C contains garbage values.

### 4.5 Submitting a task

`submit()` allocates a task node, locks the mutex, checks for shutdown, pushes
to the queue, wakes a sleeping worker, and unlocks. It must reject invalid
input such as a `NULL` task function, and it must return `-1` if the pool is
already shutting down or task allocation fails. Think about the difference
between `pthread_cond_signal` (wake one) and `pthread_cond_broadcast` (wake
all) — which is appropriate here?

---

## 5. Phase C — `thread_pool_wait()` Semantics

### 5.1 The goal

`thread_pool_wait(pool)` blocks until the pool is **idle**: the queue is empty
AND no worker is currently executing a task.

### 5.2 Why "queue empty" is not enough

Consider this timeline:

```
1. Worker takes task from queue (queue is now empty)
2. Caller calls wait()
3. wait() sees queue is empty → returns
4. Worker is still running the task!
```

This is wrong. `wait()` returned too early. You need to track work that has been
dequeued but not yet finished — separately from the queue count.

Think about what additional state you need, and when workers should notify
waiters that the pool might have become idle. If the pool is already idle,
`thread_pool_wait()` should return immediately; it should also work across
multiple batches of submissions.

> **This maps to OS concepts.** A process scheduler tracks both the run queue
> (ready to run) and currently-running processes. The system is idle only when
> both are empty.

---

## 6. Phase D — `thread_pool_destroy()` Semantics

### 6.1 The goal

Shut down the pool cleanly:

1. Let already-running tasks finish.
2. Discard queued-but-not-started tasks.
3. Wake all sleeping workers so they notice the shutdown.
4. Join all worker threads.
5. Free all resources.

`destroy(NULL)` must also be safe. The tests exercise repeated create/destroy
cycles, so shutdown cannot leave stale synchronization state behind.

### 6.2 Key design questions

- How do workers learn that shutdown has started? They are blocked in
  `pthread_cond_wait` — they won't check any flag until they wake up.

- Should you use `signal` or `broadcast` to wake workers during shutdown? Think
  about how many workers might be sleeping.

- After joining all workers, what cleanup remains? Think about the queue (it may
  still have unstarted tasks), the synchronization primitives, and the
  allocations.

> **Reverse-order cleanup** is a universal pattern. Whether you're using `goto
cleanup` in C, RAII destructors in C++, or `defer` in Go: resources acquired
> last are released first. This ensures no resource is freed while another that
> depends on it is still alive.

> In the real world, however, _when_ cleanup runs can be just as dangerous as
> _how_ it runs. The TA once fixed a TVM deadlock
> ([#18157](https://github.com/apache/tvm/issues/18157))
> triggered by Python's garbage collector invoking a pool's `shutdown()` right
> inside another pool's lock context.

---

## 7. Key Concepts

### 7.1 Mutexes: mutual exclusion

A mutex ensures that only one thread executes a critical section at a time.
Forget the lock: data race. Forget to unlock: deadlock. Hold the lock too long:
you serialize your program and lose all parallelism benefits.

### 7.2 Condition variables: event-driven waiting

A condition variable lets a thread sleep efficiently until something interesting
happens. `pthread_cond_wait` does three things atomically: unlocks the mutex,
puts the thread to sleep, and relocks the mutex when woken.

The canonical usage always wraps `pthread_cond_wait` in a `while` loop that
re-checks the condition. This is not optional — it is required by POSIX.

### 7.3 Thread safety and reentrancy

`submit()` must be callable from any thread, including from inside a running
task. This works because the worker releases the lock before executing the task
— so a task that calls `submit()` acquires the lock from a thread that doesn't
currently hold it.

> **Thread-safety vs reentrancy.** A function is thread-safe if it works
> correctly when called simultaneously from multiple threads. A function is
> reentrant if it works correctly even when called from within itself (e.g., a
> signal handler). Thread-safety usually requires locks; reentrancy usually
> requires avoiding global/static state.

### 7.4 The demo

After your implementation works, try `src/demo_attention.c` — it uses your
thread pool to parallelize multi-head self-attention, the core operation in
transformer models (the architecture behind LLMs). This is a concrete example
of how thread pools accelerate real AI workloads.

---

## 8. What Comes Next

The thread pool you just built will be integrated into the Coding Agent in the
second half of the course. When the LLM requests multiple file reads in
parallel, the agent submits each read to your thread pool. `thread_pool_wait()`
ensures all reads complete before results are sent back. `destroy()` ensures
clean shutdown.

The concurrency concepts — mutexes, condition variables, producer-consumer,
shutdown coordination — are the foundation of concurrent systems in every
language. Whether you later work with Go's goroutines, Rust's async/await, or
Python's `ThreadPoolExecutor`, the underlying problems are
the same.
