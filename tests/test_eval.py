"""Evaluation scenarios — exercise the agent end-to-end and collect metrics.

Each test runs a scripted scenario through the mock server, verifies
correctness, and reports metrics (turns, tokens) printed by the agent.
"""

from __future__ import annotations

import json
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(__file__))

from harness import (
    MockServer,
    TestCase,
    assert_contains,
    assert_exit_ok,
    find_tool_message,
    run_agent,
)

# ── Metric extraction ────────────────────────────────────────────────────


def _parse_metrics(stdout: str) -> dict[str, int]:
    """Extract the [metrics] line from stdout and return as dict."""
    m = re.search(r"turns=(\d+).*?prompt=(\d+).*?completion=(\d+).*?total=(\d+)", stdout)
    if not m:
        return {}
    return {
        "turns": int(m.group(1)),
        "prompt": int(m.group(2)),
        "completion": int(m.group(3)),
        "total": int(m.group(4)),
    }


# ── Helpers ──────────────────────────────────────────────────────────────


def workspace_with_files(*pairs: tuple[str, str]) -> str:
    d = tempfile.mkdtemp(prefix="eval_ws_")
    for name, content in pairs:
        with open(os.path.join(d, name), "w", encoding="utf-8") as fh:
            fh.write(content)
    return d


# ── Scenarios ────────────────────────────────────────────────────────────

# S1: Single bash echo
SCRIPT_BASH_ECHO = [
    {"type": "tool_calls", "calls": [
        {"id": "b1", "name": "bash", "arguments": {"command": "echo hello-world-eval"}},
    ]},
    {"type": "text", "content": "echo-done"},
]

# S2: Two-turn ReAct chain (bash → bash → text)
SCRIPT_TWO_BASH = [
    {"type": "tool_calls", "calls": [
        {"id": "b1", "name": "bash", "arguments": {"command": "echo step1"}},
    ]},
    {"type": "tool_calls", "calls": [
        {"id": "b2", "name": "bash", "arguments": {"command": "echo step2"}},
    ]},
    {"type": "text", "content": "final-done"},
]

# S3: Two parallel reads in one response
SCRIPT_PARALLEL_READS = [
    {"type": "tool_calls", "calls": [
        {"id": "r1", "name": "read_file", "arguments": {"path": "a.txt"}},
        {"id": "r2", "name": "read_file", "arguments": {"path": "b.txt"}},
    ]},
    {"type": "text", "content": "reads-done"},
]

# S4: Read → edit → read chain (3 tool turns, then final text)
SCRIPT_READ_EDIT_READ = [
    {"type": "tool_calls", "calls": [
        {"id": "r1", "name": "read_file", "arguments": {"path": "data.txt"}},
    ]},
    {"type": "tool_calls", "calls": [
        {"id": "e1", "name": "edit_file", "arguments": {
            "path": "data.txt", "old_text": "old", "new_text": "new",
        }},
    ]},
    {"type": "tool_calls", "calls": [
        {"id": "r2", "name": "read_file", "arguments": {"path": "data.txt"}},
    ]},
    {"type": "text", "content": "all-done"},
]

# S5: Write + verify (2 turns in one response + 1 text turn)
SCRIPT_WRITE = [
    {"type": "tool_calls", "calls": [
        {"id": "w1", "name": "write_file", "arguments": {
            "path": "out.txt", "content": "eval-content\n",
        }},
    ]},
    {"type": "text", "content": "write-done"},
]

# S6: Mixed bash + read_file (parallel execution — both read-only)
SCRIPT_MIXED_BASH_READ = [
    {"type": "tool_calls", "calls": [
        {"id": "b1", "name": "bash", "arguments": {"command": "echo mixed"}},
        {"id": "r1", "name": "read_file", "arguments": {"path": "a.txt"}},
    ]},
    {"type": "text", "content": "mixed-done"},
]


# ── Tests ────────────────────────────────────────────────────────────────


def test_single_bash_echo(binary: str) -> None:
    """S1: Single bash command, verify metrics."""
    with MockServer(SCRIPT_BASH_ECHO) as mock:
        result = run_agent(binary, "run echo")
        mock.inspect()

    assert_exit_ok(result)
    assert_contains(result.stdout, "echo-done")
    metrics = _parse_metrics(result.stdout)
    assert metrics.get("turns", -1) >= 1, f"expected ≥1 turn, got {metrics}"
    assert metrics.get("prompt", 0) > 0, f"expected positive prompt tokens, got {metrics}"
    assert metrics.get("completion", 0) > 0, f"expected positive completion tokens, got {metrics}"


def test_two_bash_turns(binary: str) -> None:
    """S2: Two-turn chain, verify turn count."""
    with MockServer(SCRIPT_TWO_BASH) as mock:
        result = run_agent(binary, "do two things")
        mock.inspect()

    assert_exit_ok(result)
    assert_contains(result.stdout, "final-done")
    metrics = _parse_metrics(result.stdout)
    assert metrics.get("turns", 0) >= 3, f"expected ≥3 turns for 2-hop chain (2 tool + 1 text), got {metrics}"


def test_parallel_reads_metrics(binary: str) -> None:
    """S3: Two parallel reads in one turn."""
    ws = workspace_with_files(("a.txt", "alpha\n"), ("b.txt", "beta\n"))
    try:
        with MockServer(SCRIPT_PARALLEL_READS) as mock:
            result = run_agent(binary, "read both", cwd=ws)
            inspect = mock.inspect()

        assert_exit_ok(result)
        assert_contains(result.stdout, "reads-done")

        second = inspect["requests"][1]["messages"]
        r1 = find_tool_message(second, "r1")
        r2 = find_tool_message(second, "r2")
        assert_contains(r1["content"], "alpha", label="r1")
        assert_contains(r2["content"], "beta", label="r2")

        metrics = _parse_metrics(result.stdout)
        assert metrics.get("turns", 0) >= 1, f"expected ≥1 turn, got {metrics}"
    finally:
        import shutil
        shutil.rmtree(ws, ignore_errors=True)


def test_read_edit_read_chain(binary: str) -> None:
    """S4: Three-turn read→edit→read chain."""
    ws = workspace_with_files(("data.txt", "hello old world\n"))
    try:
        with MockServer(SCRIPT_READ_EDIT_READ) as mock:
            result = run_agent(binary, "edit the file", cwd=ws)
            mock.inspect()

        assert_exit_ok(result)
        assert_contains(result.stdout, "all-done")

        with open(os.path.join(ws, "data.txt"), encoding="utf-8") as fh:
            content = fh.read()
        assert_contains(content, "new", label="edit result")

        metrics = _parse_metrics(result.stdout)
        assert metrics.get("turns", 0) >= 4, f"expected ≥4 turns for 3-hop chain, got {metrics}"
    finally:
        import shutil
        shutil.rmtree(ws, ignore_errors=True)


def test_write_file_metrics(binary: str) -> None:
    """S5: Single write_file turn."""
    ws = workspace_with_files()
    try:
        with MockServer(SCRIPT_WRITE) as mock:
            result = run_agent(binary, "write it", cwd=ws)
            mock.inspect()

        assert_exit_ok(result)
        assert_contains(result.stdout, "write-done")

        target = os.path.join(ws, "out.txt")
        if not os.path.isfile(target):
            raise AssertionError("write_file did not create out.txt")

        metrics = _parse_metrics(result.stdout)
        assert metrics.get("turns", 0) >= 1
        assert metrics.get("total", 0) > 0
    finally:
        import shutil
        shutil.rmtree(ws, ignore_errors=True)


def test_mixed_bash_read_serial(binary: str) -> None:
    """S6: Mixed bash + read — falls back to serial (bash is not read-only)."""
    ws = workspace_with_files(("a.txt", "file-a\n"))
    try:
        with MockServer(SCRIPT_MIXED_BASH_READ) as mock:
            result = run_agent(binary, "mixed", cwd=ws)
            inspect = mock.inspect()

        assert_exit_ok(result)
        assert_contains(result.stdout, "mixed-done")

        second = inspect["requests"][1]["messages"]
        b1 = find_tool_message(second, "b1")
        assert_contains(b1["content"], "mixed", label="bash output")

        metrics = _parse_metrics(result.stdout)
        assert metrics.get("turns", 0) >= 1
    finally:
        import shutil
        shutil.rmtree(ws, ignore_errors=True)


def get_cases() -> list[TestCase]:
    return [
        TestCase(name="single_bash_echo",          phase="eval", points=5,  func=test_single_bash_echo),
        TestCase(name="two_bash_turns",             phase="eval", points=5,  func=test_two_bash_turns),
        TestCase(name="parallel_reads_metrics",     phase="eval", points=5,  func=test_parallel_reads_metrics),
        TestCase(name="read_edit_read_chain",       phase="eval", points=5,  func=test_read_edit_read_chain),
        TestCase(name="write_file_metrics",         phase="eval", points=5,  func=test_write_file_metrics),
        TestCase(name="mixed_bash_read_serial",     phase="eval", points=5,  func=test_mixed_bash_read_serial),
    ]
