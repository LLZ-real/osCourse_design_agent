# Project 2 Part 2: Tool Registry & Parallel Execution

Your Part 1 agent already works. The goal of Part 2 is to evolve the system
while keeping its external behavior intact。

## The system problem

Part 1 has two design pressures.

First, tool knowledge is spread across the system. The agent loop knows how
to dispatch a concrete tool; the LLM client knows how to describe that tool
on the wire; the tool header exposes implementation facts that only some
modules should need. This is manageable for one tool and brittle for four
and more.

Second, the executor treats a batch of tool calls as a flat list. Three
independent reads are serialized even though one read depends on nothing
from another. The fix for this requires more than running everything in
parallel: writes, edits, unknown tools, and message ordering all still
matter.

By the end of this part, the system should have one path for tool discovery,
one path for tool dispatch, and one executor contract that can exploit
read-only parallelism while preserving what the LLM sees.

> **Tips: stable interface, replaceable implementation**
>
> Part 2 changes how the agent is built internally. The external contract —
> talk to the LLM, run tools, push results into history — stays the same.
> This is the same kind of separation an OS tries to maintain between a
> stable external interface and kernel-internal implementation choices.

---

## 1. Starting point

Start from your working Part 1 tree. The Part 2 package provides new
framework pieces and tests. Copy the files into your Part 1 tree. After that,
the project may fail to build. That is expected. The new framework code refers
to interfaces that your Part 1 code has not yet learned.

The full build and test targets are listed at the end. During Phase A,
expect `make test-a` and `make test-cunit` to become meaningful first;
during Phase B, move to `make test-b` and the TSan executor tests.

The package provides:

- **Framework code**: `tools/registry.c`, `tools/executor.{h,c}`,
  `tools/sandbox.{h,c}`
- **Tests**: `tests/test_part2_a.py`, `tests/test_part2_b.py`,
  `tests/cunit/{test_registry,test_sandbox,test_executor}.c`, and an
  updated test harness (`harness.py`, `run_tests.py`). The updated harness
  is backward compatible: your Part 1 tests continue to work.
- **Build configuration**: `Makefile` with all Part 2 targets. This is the
  grading build file. If your Part 1 Makefile has local fixes, merge them
  into this one.

---

## 2. Phase A — One Interface for Many Tools

The external behavior stays the same. What changes is ownership.

After this phase, the agent loop should be independent of which tools exist.
The LLM client should discover tools from the registry. A concrete tool
should own its runtime implementation and the metadata needed to advertise
it.

### The interface

Your Part 1 `tools/tools.h` is one of the boundaries that must evolve. At
the start of Phase A, the framework needs the following common tool
interface:

```c
typedef struct {
    bool ok;
    char *output;        /* heap-allocated; freed by tool_result_free */
} ToolResult;

void tool_result_free(ToolResult *r);

#define MAX_TOOL_OUTPUT 50000

typedef ToolResult (*ToolFn)(cJSON *args);

typedef struct {
    const char *name;
    const char *desc;
    const char *param_schema;
    ToolFn exec;
} ToolDef;

#define MAX_REGISTERED_TOOLS 16

void tools_init(void);
void tool_register(ToolDef *def);
ToolDef *tool_find(const char *name);
ToolDef *const *tool_list(int *out_count);
```

Before Phase A is complete, you will extend this same struct with one scheduling field.

These types are the contract between your tools, the registry, and the
executor. `ToolDef` bundles everything the system needs to know about a
tool: its wire name, the description the LLM reads, the JSON schema for its
arguments, and the function that runs it. `ToolResult` is a small value type
so every return path in every tool must construct one explicitly.

> **Tips: dispatch tables**
>
> A registry is a dispatch table. The caller does not need one branch per
> implementation. Syscall tables, VFS operation tables, and device driver
> interfaces all use this shape: keep the caller stable while concrete
> operations vary.

### Read the provided boundaries

Start by reading the provided framework files. They define the contracts
that the implementation and tests agree on.

**`tools/registry.c`** implements the registry. It stores `ToolDef` pointers
in a flat array, populated once at startup. Read it to understand how the
system turns a registered definition into something the executor can find
later.

**`tools/executor.h`** defines `executor_run_tools` — the function the agent
calls to run a batch of tool calls. Read the contract comment: it specifies
what the executor promises about ordering and how execution failures are
reported.

**`tools/executor.c`** implements that contract. In Phase A, it runs tools
serially in request order. Read the `ToolTask` struct, the `run_one`
function, and the result-message construction loop — these are the paths
your file tools will travel through.

**`tools/sandbox.h`** is the workspace containment boundary.
`resolve_workspace_path` canonicalizes a relative path and rejects anything
that resolves outside the workspace directory.

> **Tips: protection boundary**
>
> The sandbox is the boundary between LLM-supplied paths and the filesystem.
> A request from a higher layer is a request; the runtime owns enforcement.
> If each tool reimplements path validation, the boundary has leaked.
> In a real system, the sandbox would be more complex and robust.

### Wrapping bash as a ToolDef

Your `tools/bash.c` has a working fork/exec implementation. Wrap it
in a `ToolDef` so the registry can find it through a uniform interface.

```c
ToolDef bash_def = {
    .name = "bash",
    .desc = "Run a shell command and return its combined stdout/stderr.",
    .param_schema = "{\"type\":\"object\","
                    "\"properties\":{\"command\":{\"type\":\"string\","
                    "\"description\":\"The shell command to execute\"}},"
                    "\"required\":[\"command\"]}",
    .exec = tool_bash,
};
```

Your file tools follow the same shape: one `ToolDef` per tool, with the
implementation function reached through the `.exec` pointer.

### File tool

Create `tools/read.c`, `tools/write.c`, and `tools/edit.c`. Read
`tools/registry.c` to see what symbols the registry expects to link against.

Every file tool must validate its `path` argument through
`resolve_workspace_path` before opening anything. The sandbox check is the boundary
that prevents a path like `read_file("../../etc/passwd")` from becoming a real disclosure.

The three tools:

- **`read_file`** — argument `{"path": "...", "limit"?: N}`. Returns the
  workspace-relative file's contents (UTF-8). On a `limit`, truncate
  after that many lines. Cap at `MAX_TOOL_OUTPUT` regardless. On failure
  (missing file, sandbox rejection, …), return `ok=false` with a short,
  LLM-readable message. This is read-only with respect to workspace state.

- **`write_file`** — argument `{"path": "...", "content": "..."}`. Replace
  the file's contents with `content`. The parent directory must already
  exist; tools do not `mkdir -p`. Return a message that names the file
  and the byte count on success.

- **`edit_file`** — argument
  `{"path": "...", "old_text": "...", "new_text": "..."}`. Replace the
  first exact occurrence of `old_text` with `new_text` in the file. If
  there is no match, fail. The contract is "first match".

The schemas are provided below.

`read_file`:

```json
{"type":"object","properties":{"path":{"type":"string","description":"Relative path inside the workspace"},"limit":{"type":"integer","description":"Optional maximum number of lines to return"}},"required":["path"]}
```

`write_file`:

```json
{"type":"object","properties":{"path":{"type":"string","description":"Relative path inside the workspace"},"content":{"type":"string","description":"Full file contents to write"}},"required":["path","content"]}
```

`edit_file`:

```json
{"type":"object","properties":{"path":{"type":"string","description":"Relative path inside the workspace"},"old_text":{"type":"string","description":"Exact substring to find"},"new_text":{"type":"string","description":"Replacement text"}},"required":["path","old_text","new_text"]}
```

### Connecting the system

Once your tools compile and export their `ToolDef` symbols, three things
still need to happen for the system to work end-to-end.

The registry must know about the new tools. `tools/registry.c` is where
every tool enters the system — read it and register yours. The registry
must be populated before the first LLM request asks which tools exist; this
is a boot-order dependency in `main.c`.

> **Tips: boot order**
>
> Subsystems have initialization dependencies. A filesystem cannot mount
> before its driver is registered; a device cannot be opened before the
> device table knows about it. Here, the LLM client cannot advertise tools
> before the registry has been populated.

The LLM client should discover tool schemas from the registry. The wire
format still expects one function entry per tool, with the schema inserted
as raw JSON. Walk `tool_list()` in your `build_tools_json` and emit one
entry per `ToolDef`.

After the assistant message has been appended to history, the agent should
hand the complete batch of tool calls to the executor and push the returned
tool messages into history in request order. Read the `executor_run_tools`
contract in `executor.h` to see what the agent passes in and what it gets
back.

A useful way to check your design:

- When adding another tool later, which existing files would need to change?
- If the answer includes the agent loop, which decision is still in the
  wrong module?
- If the answer includes the LLM client, where should tool metadata live
  instead?
- If a tool is unknown at runtime, which layer should turn that into an
  observation rather than a crash?

Avoid adding another `if (strcmp(name, ...))` path in the agent loop. That
may pass one tool test, but it leaves the original design pressure in place.

### Extend `ToolDef` with `read_only`

Before finishing Phase A, extend the tool interface with scheduling
metadata. Phase B needs the executor to distinguish tools that only observe
state from tools that may change state. Add one boolean field to `ToolDef`:

```c
bool read_only;
```

Use designated initializers (`.read_only = true`) in every `ToolDef`
literal so field order does not become a hidden dependency. `read_file` is
the only read-only tool in this system. `write_file` and `edit_file` change
workspace state. `bash` is treated as state-changing because the runtime
cannot know what an arbitrary shell command will do.

> **Tips: interface metadata**
>
> An interface carries more than a function pointer. It can carry metadata
> that changes what the runtime is allowed to do. File modes, page
> permissions, process states, and device capabilities all influence
> scheduling or protection decisions. Here, `read_only` is small metadata
> with runtime consequences.

### What Phase A tests check

`tests/test_part2_a.py`:

- **`tools_advertised`**: the wire-format request to the mock contains the
  four tools `bash`, `edit_file`, `read_file`, and `write_file`. If the LLM
  client still returns an empty tool list, this catches it immediately.
- **`bash_through_registry`**: the Part 1 bash tool still works through the
  new dispatch path.
- **`read_file`**, **`write_file`**, **`edit_file`**: each exercises one
  tool against a temporary workspace. The agent's stdout, the workspace
  file, and the next request to the mock are all checked.
- **`sandbox_rejects_escape`**: a `read_file` for `"../etc/passwd"` returns
  a sandbox error to the LLM. If the host's password file leaks into the
  tool message, the boundary has failed.
- **`unknown_tool_via_registry`**: a tool name the registry does not
  recognize becomes a tool observation that the LLM can read and reason
  about.

`make test-cunit` exercises the registry and sandbox at the unit level with
no LLM involvement. If the registry returns the wrong pointer or the
sandbox accepts an empty path, these fail before any end-to-end test does.

---

## 3. Phase B — Scheduling Tool Batches

A batch of tool calls has two orders.

The first is **request order**: `tool_calls[i]` must produce `out_msgs[i]`.
This is part of the protocol with the LLM and must hold regardless of how
tools execute internally.

The second is **completion order**: when independent work runs concurrently,
the first tool to finish may not be the first tool in the request. The
executor may exploit that internally, but completion order must stay
internal to the executor.

> **Tips: completion order and commit order**
>
> Concurrent work may finish in any order, but externally visible state
> often needs a different order. CPUs may execute out of order but retire
> in order; storage systems may reorder internal work while preserving
> consistency points. The executor has the same obligation: completion
> order is an internal optimization, message order is an external contract.

### The scheduling rule

The executor needs a small amount of semantic information to decide when
overlap is safe. For this lab, the useful distinction is **read-only vs
state-changing**.

When the batch has N ≥ 2 tool calls and every tool in the batch is
registered and `read_only`, the executor dispatches them concurrently and
waits for all to complete. The total wall time should move toward the
longest single call rather than the sum of all calls, subject to scheduling
overhead.

When any tool in the batch is missing, unknown, or state-changing, the
executor falls back to serial execution. This is the entire scheduling
policy.

> **Tips: conservative concurrency**
>
> More parallelism is not automatically a better design. A scheduler that
> tries to prove every pair of operations independent can become harder to
> trust than the work it speeds up. The rule in this lab fits in one
> sentence, is testable, and is auditable. Make the common safe case fast;
> keep the dangerous case simple.

### Implementing the concurrent path

Open `tools/executor.c` and read the existing serial loop and the `ToolTask`
structure. The policy decision is small; the correctness work is in
preserving the executor contract while tasks overlap.

The `ToolTask` array already has one slot per tool call. Each task has an
index, a definition pointer, and a result field. These slots are the
contract between the serial and concurrent paths: regardless of how tasks
execute, `tasks[i].result` is where the result for tool call `i` lands.

The intended architecture is to reuse your Lab 3-style thread pool as a
runtime component. If you choose that route, add the source and include flags
needed by your implementation to the build. A direct `pthread_create` /
`pthread_join` implementation can satisfy the executor contract, but it will
not earn full scores; the final score distinguishes these designs by source
review.

A few invariants that make this work:

1. **Each task owns its slot.** Worker `i` writes only `tasks[i].result`.
   The main thread reads all slots only after every worker has completed.

2. **Tools have no shared mutable state.** `read_file` opens its own file
   handle, allocates its own buffer, returns a fresh `ToolResult`.

### What Phase B tests check

`tests/test_part2_b.py`:

- **`three_parallel_reads_in_order`**: three `read_file` calls in one
  response. Contents go back to the LLM in request order regardless of
  completion order.
- **`mixed_read_write_serial_fallback`**: a response combining reads and a
  write. All succeed, the written file exists on disk, and messages remain
  in request order.
- **`eight_parallel_reads_complete`**: 8-call batch. Catches "forgot one
  worker" bugs.
- **`parallel_speedup_observed`**: coarse timing check. Eight reads of a
  32 KB file should take less than 4× a single read. The bound is generous;
  if scheduling is silently serial, this still fails.

`make test-cunit-parallel` and `make test-cunit-tsan` exercise the executor
under ASan and TSan respectively.

---

## 4. Build and Test

```bash
make                  # default build → build/c-agent
make test-a           # Phase A integration tests
make test-b           # Phase B integration tests (needs Phase A done first)
make test             # all phases (including Part 1 tests if present)
make asan             # ASan + UBSan build
make test-asan        # all tests under ASan
make tsan             # ThreadSanitizer build
make test-tsan        # C-level executor test under TSan

make test-cunit            # pure-C unit tests (registry, sandbox)
make test-cunit-parallel   # ASan run of the parallel executor unit test
make test-cunit-tsan       # TSan run of the parallel executor unit test
make clean
```

The mock server (`tests/mock_server.py`) works the same as Part 1.

### Manual experimentation

To drive the agent against a real LLM (Caddy proxy on `:18080`, your
`API_KEY` exported):

```bash
mkdir -p /tmp/agent-scratch && cd /tmp/agent-scratch
/path/to/build/c-agent
> read README.md and edit_file the typo on line 12 to fix it
```

If the LLM returns a mixed batch such as `read_file` followed by
`edit_file`, the executor should keep that batch serial. If it returns
several `read_file` calls in the same assistant message, their spinners
should appear simultaneously.

---

## 5. What Comes Next

Part 3 introduces context-window management: when the message list grows
past the LLM's input budget, the agent has to decide what to drop, what to
summarize, and what to keep. The registry you built here will be the
catalogue Part 3 reasons over.

The concurrency path you integrated will stay stable; the parallelism
appetite will grow. As the agent runs tasks like "read every C file in the
project", it issues ten- or twenty-call batches, and the difference between
"all in parallel" and "one at a time" becomes visible in the user
experience. The same question will return: which information belongs in the
model's context, and which belongs in the runtime's state?

> The model may request work. The runtime decides what is safe, bounded,
> ordered, and observable.
