# Project 2 (Part 1): A Basic AI Agent

You already have the three mechanisms that a coding agent depends on: a
shell-style fork/exec pattern for running child programs, an HTTP client for
talking to a remote service, and a thread pool for running work
concurrently. What none of these pieces can do, on its own, is decide what
to do next.

This project changes that. You will wire the pieces together and give the
resulting program a _decider_ — a remote LLM that, turn by turn, chooses
the next tool to run and the next thing to say. When the LLM asks for a
shell command, your program forks a child, runs it, captures the output,
and sends the output back. When the LLM has no more tools to run, your
program prints the final answer.

The end product is a coding assistant that lives in your terminal. It reads
your request, runs real commands against real files, and reports what it found.
It is also the substrate for Parts 2 and 3 (which add a tool registry,
parallel execution, and context management), so the structural decisions
you make here will be lived with.

---

## 1. What You Are Building

| Phase | New capability                          | Core loop shape                                              |
| ----- | --------------------------------------- | ------------------------------------------------------------ |
| A     | One user prompt → one tool call → reply | `read → ask LLM → run tool → ask LLM → print`                |
| B     | Multiple tool calls per prompt (ReAct)  | wrap Phase A in a `while` that breaks when the LLM stops     |
| C     | Multi-turn dialogue + live TUI          | outer REPL over user inputs, spinner + per-tool status lines |

---

## 2. Before You Start

### 2.1 Directory layout

```
project2/
├── handout.md
├── Makefile
├── main.c
├── config.{h,c}
├── http.{h,c}
├── message.{h,c}
├── util.{h,c}
├── agent/
│   ├── agent.{h,c}
│   └── llm_client.{h,c}
├── tools/
│   ├── bash.c
│   └── tools.h
├── ui/
│   ├── ui.c
│   ├── ui.h
│   ├── render.c
│   └── internal.h
├── libs/
│   └── cJSON.{h,c}
└── tests/
    ├── mock_server.py
    ├── run_tests.py
    ├── harness.py
    ├── test_phase_a.py
    ├── test_phase_b.py
    └── test_phase_c.py
```

Working boundaries:

- You primarily edit `agent/`, `tools/bash.c`, and `main.c`.
- `ui/`, `libs/`, and `tests/` are given. Read them; do not rewrite them.
- The `Makefile` auto-discovers `.c` files under `agent/`, `tools/`, and
  `ui/` via wildcard — adding a new source file in a later phase just
  works.

### 2.2 Build and test

```bash
make               # default binary: build/c-agent
make test          # runs all phase tests
make test-a        # just Phase A
make test-b
make test-c
make asan          # AddressSanitizer build (build/c-agent-asan), no tests
make test-asan     # run all tests under ASan
make clean
```

Tests always build first, so `make test-a` alone is enough during daily
work.

### 2.3 The mock LLM server

`tests/mock_server.py` is a tiny Python `http.server` that pretends to be
the LLM API. You never have to run it yourself — the test harness spawns a
fresh instance per test with a scripted deck of responses.

### 2.4 Running against the real LLM

For manual experimentation you can point the agent at a real proxy: run
`caddy reverse-proxy --from :18080 --to https://models.sjtu.edu.cn`,
export your `API_KEY`, and set `LLM_HOST=127.0.0.1 LLM_PORT=18080`.

### 2.5 Testing your agent by actually using it

The phase tests tell you your wire protocol is right. They do not tell
you whether the agent is _useful_. You can try it:

```bash
mkdir -p /tmp/agent-scratch && cd /tmp/agent-scratch
export API_KEY=...   # see 2.4
/path/to/build/c-agent
> write a C program that prints the first 20 primes to primes.c, then compile and run it
```

A working Phase A agent will `bash` out a few commands — write the file,
invoke `gcc`, run the binary — and report the output. It is the fastest
way to notice problems the mock cannot catch:

- the LLM's tool call reaches your dispatch in a form you can actually
  parse (newlines, nested quotes, heredocs…)
- long tool outputs do not destabilize the next request body
- failed commands (compile errors) come back as signal for the LLM, not
  as silent successes

---

## 3. Phase A — Single-Turn Tool Call

### 3.1 The goal

Type one line, the agent uses `bash` to gather information, the agent answers.

```
$ ./build/c-agent
How many C files are in /etc?
14
```

Under the hood, five things had to happen:

```
user line ─┐
           ▼
   +----------------+      +-------------+      +-----------+
   |   agent.c      │ ───▶ │ llm_client  │ ───▶ │   LLM     │
   |  (orchestrate) │      │  build HTTP │      │           │
   +----------------+      +-------------+      +-----------+
           ▲                                            │
           │              (returns: tool_call bash)     │
           │                                            ▼
   +----------------+      +-------------+      +-----------+
   │   print(reply) │ ◀─── │  llm_chat   │ ◀─── │ bash.c    │
   │                │      │  (2nd call) │      │ fork/exec │
   +----------------+      +-------------+      +-----------+
```

### 3.2 What is given

- `http.{h,c}` — HTTP primitives: `tcp_connect`, `send_all`, `recv_all`,
  `http_parse_response`.
- `message.{h,c}` — a dynamic array of JSON-serialized messages and
  constructors for `user` / `tool` messages.
- `tools/tools.h` and `tools/bash.c` — schema globals for the `bash`
  tool, plus the child-side of `bash_tool_exec` (pipe + fork + dup2 +
  execl). You fill in the parent side.
- `main.c` — reads one line, calls `agent_chat`, prints the reply, exits.
- `libs/cJSON` — a minimal JSON library.

### 3.3 What you implement

Three places, each marked with a TODO in the source:

**`tools/bash.c` — the parent side of `bash_tool_exec`.**

After fork you own `pipefd[0]` (the read end) and `pid`. Turn that into a
`ToolResult`: decide how much output to keep, whether the command
succeeded, and what string the LLM sees on the next turn. A failed
command is still useful information — non-zero exit statuses and fatal
signals tell the LLM that its command did not do what it expected, so
include that in `.output` rather than swallowing it. See `waitpid(2)` and
the `WIFEXITED` / `WEXITSTATUS` / `WIFSIGNALED` / `WTERMSIG` macros in
`<sys/wait.h>`. Close `pipefd[0]` and reap the child before returning.

**`agent/llm_client.c` — `llm_chat`.**

1. Build the request body. It is a JSON object with these fields:
   `model`, `messages` (an array containing the system prompt plus the
   given message list), `tools` (an array with one entry describing
   `bash`), and `max_tokens`. Use `cJSON` to construct it — do not hand-
   splice strings.
2. Build the HTTP request, `tcp_connect`, `send_all(header)` then
   `send_all(body)`, `recv_all`, `http_parse_response`.
3. Reject non-200 status codes — write a message into `err` and return -1.
4. Parse the response body: `choices[0].message` is the assistant message.
   Read its `content` (may be empty or absent) and its `tool_calls` array.
   For each tool call, capture `id`, `function.name`, and
   `function.arguments` (a JSON _string_ on the wire; parse it back into
   an object with `cJSON_Parse`).
5. Keep a serialized copy of the whole assistant message (`raw_message`) —
   the agent will push it back into history verbatim so the LLM sees its
   own previous reply on the next call.

**`agent/agent.c` — `agent_chat`.**

1. Push a `user` message onto a local `MessageList`.
2. Call `llm_chat`. On failure, return NULL.
3. If the response has no tool calls, print and cache its `content` and
   return it. Done.
4. Otherwise: push `response.raw_message` onto history; execute the tool
   the LLM asked for; push the tool result as a `tool` message with the
   right `tool_call_id`; call `llm_chat` again; print and return the
   final content.

### 3.4 Edge cases that are yours to handle

- **No tool arguments:** the LLM can emit a tool call with an empty
  `arguments` string. `cJSON_Parse("")` returns NULL; treat that as
  `cJSON_CreateObject()`.
- **Failed shell commands:** a command that exits non-zero or dies to a
  signal is not an error in the agent — it is an observation the LLM
  needs to see so it can recover. Encode the exit status into the tool
  output rather than returning a generic "bash failed" message.
- **Unknown tool name:** the test suite will send a tool call the agent
  has never heard of. Your dispatch must reject it with a `ToolResult`
  whose `.output` names the unknown tool, so the LLM can see its mistake
  and try again, rather than the program aborting.

---

## 4. Phase B — The ReAct Loop

### 4.1 The goal

Phase A only lets the LLM take one action. Real tasks ("find the file that
defines `foo`, then count the lines in it") need a chain: read, think,
read again, think, answer. The pattern is called **ReAct** (Reason +
Act): the LLM reasons out loud, takes an action (tool call), observes the
result, reasons some more, acts again, and eventually returns a final
answer.

```
          ┌────────────────────┐
  user ──▶│     agent_chat     │
          │                    │
          │   ┌────────────┐   │
          │   │ llm_chat   │◀──┼──────────┐
          │   └────┬───────┘   │          │
          │        │           │          │
          │  n_tool_calls?     │          │
          │   ┌────┴────┐      │          │
          │  =0         >0     │          │
          │   │         │      │          │
          │   ▼         ▼      │          │
          │ print    run each ─┼─► push ──┘
          │          tool      │    tool
          │                    │    result
          └────────────────────┘
```

### 4.2 What changes

You edit the two files you already own:

- `agent/llm_client.{h,c}`: in Phase A you only had to parse one tool
  call per response. Now you have to parse all of them. Revisit how
  `LLMResponse` stores its `tool_calls` — a fixed-size array is fine if
  you pick a cap you can defend; a growing buffer is fine too. Either
  way, update `llm_chat` to parse every entry in request order.
- `agent/agent.c`: wrap the body of `agent_chat` in a loop. Terminate
  when the response has zero tool calls. Within one iteration, execute
  every tool call in the response and push their results in request
  order. This is also a good moment to look at the `struct Agent` you
  wrote in Phase A — the multi-iteration loop may want state you did
  not previously need.

### 4.3 Why "in request order"

If the LLM asks for calls `t1` and `t2` in a single response, the tool
messages must appear in history in that order even if the _execution_
order is different. In Phase B execution is sequential so this is
automatic; Part 2 will execute them in parallel and the ordering
invariant will become a real design constraint. Write the code now in a
way that does not assume serial execution, so you do not have to revisit
it later.

### 4.4 Termination as an invariant

What guarantees the loop ends? The LLM can, in theory, keep asking for
tools forever. Two lines of defense are normal:

1. A hard iteration cap (`MAX_TURNS`, say 20). If you exceed it, report
   an error and stop. This is how every production agent protects itself.
2. A sane system prompt that nudges the LLM toward finishing. You do not
   need to tune this now — the mock tests terminate by ending their
   scripts.

A failed tool execution is _still_ a valid tool result. Push the failure
message back into history and let the LLM react; do not abort the loop.

### 4.5 What the tests check

- Three-hop chain: mock scripts three successive `bash` calls, then a
  final text. Your agent must produce four requests to the mock and
  preserve every tool result in the growing history.
- Two tool calls in a _single_ LLM response: both execute, both come
  back in order.

---

## 5. Phase C — Multi-Turn Dialogue and the TUI

### 5.1 The goal

One shot is fine for a demo; a real agent holds a conversation. A user
asks something, gets an answer, asks a follow-up, gets a better answer
because the agent still remembers the first turn. At the same time we
finally start showing the user _what the agent is doing_ while it runs —
the spinner, the per-tool status line, the timing.

### 5.2 What you implement

**`main.c` — turn the single-shot driver into a REPL.**

- Call `ui_init()` at startup.
- Print a banner.
- Loop: read a line from the user, pass it to `agent_chat()`, print.
- Exit on `exit`, `quit`, `q`, or EOF.

**`agent/agent.c`**

1. Persist history across calls. This is the third time you are
   revisiting `struct Agent`; the history you allocated per-call in
   Phases A and B now needs to live on the agent itself and be freed in
   `agent_free`.
2. **Emit UI events at the right moments.** The public contract is in
   `ui/ui.h`. Roughly:
   - Before every `llm_chat` call, emit `ui_begin_thinking()`. The render
     thread draws a spinner until you signal otherwise.
   - When you are about to run tools, build a small stack array of
     `ToolCallView` values (name + short argument display) and call
     `ui_begin_tools(n, views)`.
   - After each tool completes, call `ui_tool_done(index, ok, output)`.
   - Before `printf`'ing the assistant's final text from the main
     thread, call `ui_idle()`. It is a _barrier_: it blocks until the
     render thread has released the dynamic region.

### 5.3 The synchronization question

You now have two threads that both want to write to the terminal: your
main thread (agent logic, LLM calls, tool dispatch) and the render thread
(spinner animation, per-tool status frames). Without discipline they
would interleave mid-line and the display would corrupt.

The `ui/` framework solves this by restricting the main thread to a
small event-posting API and letting the render thread own stdout for the
"dynamic region" (currently-running work). `ui_idle()` is the barrier
between the two regimes: the main thread waits on a condition variable,
the render thread acknowledges, and _then_ the main thread is free to
`printf`.

### 5.4 Tests

- **Multi-turn history**: two user prompts in one session. The mock
  records two distinct requests; the second one must contain both user
  messages in order, so the LLM sees what came before.
- **Clean exit**: `exit` terminates the program within a couple of
  seconds. The render thread must be joined (see `ui_stop`) — a
  lingering thread would block `main` from returning and the test would
  time out.
