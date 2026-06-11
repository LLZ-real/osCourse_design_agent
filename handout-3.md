# Project 2 Part 3: Context Management

When an agent runs a long task (walking a directory tree, reading many files, engaging in extended conversations) the flat list of messages it accumulates quickly becomes a burden. Every turn adds the user prompt, the model response, tool calls, and tool results. Without management, three pressures accumulate:

1. **Window limits.** Once the total token count exceeds the model’s context window, the next request is rejected and the conversation fails.
2. **Cost.** Tokens are billed. A monotonically growing history forces every turn to retransmit old content at increasing expense.
3. **Quality.** Attention is a finite resource. A long, cluttered history rarely produces answers as good as the same task framed within a tight, recent window.

This part introduces an explicit **Context** subsystem that keeps the agent’s message history within a manageable budget.

---

## 1. The Context Interface

The public surface lives in `context/context.h`. Each policy carries a name, a condition that decides when it should apply, and an action that reclaims context space. Before any model request the agent invokes `ctx_reclaim`, which walks the registered policies in registration order.

Inside the agent implementation (`agent/agent.c`), the native message list is replaced by a `Context` pointer. Every addition to the history routes through `ctx_push`. Every call to the model is preceded by a successful `ctx_reclaim` and receives the up‑to‑date history from `ctx_history(ctx)`.

Three environment variables configure the behaviour:

- `CONTEXT_WINDOW` – the model’s total token budget.
- `OFFLOAD_THRESHOLD` – the fraction of the window that triggers the offload policy.
- `SUMMARY_THRESHOLD` – the fraction of the window that triggers the summary policy.

---

## 2. Offload Policy

Long tool outputs are bulky but losslessly recoverable: the full payload already lives on disk or can be reproduced by rerunning the tool. The offload policy therefore moves the body out of the conversation and leaves behind a compact pointer, much like a preview with a recovery hint. This is ideal for workloads with large tool results where the agent rarely needs the full content but must be able to fetch it on demand.

**Trigger.** The policy activates when the current token usage, measured by `ctx_budget_usage(ctx)`, exceeds the `OFFLOAD_THRESHOLD` fraction of the context window.

**Action.** It inspects tool‑role messages that lie outside the most recent `KEEP_RECENT_MSGS` messages. For each qualifying message whose body is sufficiently long, the original content is saved to disk under `.agent/offload/`. The message body is replaced with a short placeholder that contains the substrings `"read_file"` and `".agent/offload/"`, signalling that the full payload can be retrieved with the `read_file` tool at the given path.

---

## 3. Summary Policy

When the bulk of the conversation stems from many back‑and‑forth turns rather than tool output, a lossy strategy becomes necessary. The summary policy asks the model itself to compress old history into a single handoff message, retaining only the essential facts, decisions, and outcomes. This approach works well for dialogues that need to preserve a thread of context but can discard verbatim details.

**Trigger.** The policy activates when the token usage exceeds the `SUMMARY_THRESHOLD` fraction of the window. However, if the total message count is not greater than `KEEP_RECENT_MSGS`, the policy does nothing and **must not** call the model.

**Action.** Otherwise the policy makes exactly one LLM request using `g_config.model`. The request sends the prefix of messages that should be collapsed, optionally together with a running summary for an incremental merge.

On success, all messages before the most recent `KEEP_RECENT_MSGS` are replaced by a single message whose role is `user` and whose content is the model’s summary. A short header on the body is acceptable. The most recent messages are preserved verbatim.

---

## 4. Policy Collaboration

The two policies are intended to work together, and the order matters. The offload policy runs first because it is a lossless, inexpensive operation that trims bulk from tool results without altering the semantics of the conversation. The summary policy runs second, handling the remaining verbosity in the dialogue flow itself.

Because each policy independently checks its own threshold against the same token budget, a single `ctx_reclaim` pass may trigger both. The system guarantees that the offload step reduces the token count before summary evaluates the budget, which helps avoid unnecessary compression. This staged design keeps the framework open: additional policies can be added later simply by registering them with a well‑defined priority, and the agent core remains unchanged.

---

## 5. Build and Test

Compile the project and run the unit tests:

```bash
make test-cunit-offload
make test-ca
make test-cunit-summary
make test-cb
```

---

## 6. Extension

You need to select at least one extension (more as a bonus) to complete, and specify in the report which ones you have completed.

### 6.1 Evaluation

After building an agent, the natural next step is to see what it can do. Design a few scenarios that exercise your agent end-to-end and observe its behavior. For example, recursively find all `.c` files in a directory tree, explore an unfamiliar codebase and answer a specific question about it or perform a precise multi-file text replacement without damaging unrelated code. Metrics worth tracking: success rate per scenario, rounds of tool calls per task, prompt and completion token consumption. The numbers give you a concrete picture of how your agent behaves.

### 6.2 SubAgent

When a single agent handles several independent things at once (reading code, searching docs, running a build), those threads compete for the same context window. The model's attention is divided, and more critically, every subtask's intermediate output consumes tokens from the shared budget. Offloading independent work to child agents keeps the main context lean. Design tools that enable the main agent to spawn child agents for bounded subtasks. Each child gets its own context window, executes, and returns a structured result. The parent receives the result and continues.

### 6.3 Session

A crash or a closed terminal wipes all in-memory conversation state. The idea is an append‑only log of the conversation written to stable storage. On startup the agent replays the log to rebuild history. Interrupted sessions resume where they left off. Several commands are needed to implement this mechanism.

### 6.4 Memory

Context management handles history within a session. But the agent also produces knowledge that outlives any single session: project structure, build conventions, coding style, decisions from past conversations. A project-level memory gives the agent continuity. It learns what matters in this codebase, and can be saved to `.agent` folder and loaded in future sessions. Design tools that let the agent write to and read from the memory store.

### 6.5 Skill

A general-purpose agent can do anything but excels at nothing. When asked to review code, it should know what to check, in what order, and how to format the output, without the user spelling it out each time. A skill is a prompt that guides the model through a specific kind of task, telling it what steps to follow, what to look for, and what output shape to produce. The challenge is token cost. Progressive disclosure reduces this overhead: at startup, inject only skill names and one-line descriptions. When the LLM identifies a relevant skill, it calls a tool to load the full prompt on demand.
