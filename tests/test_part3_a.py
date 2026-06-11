from __future__ import annotations

import os
import shutil
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


def workspace() -> str:
    return tempfile.mkdtemp(prefix="agent_offload_")


# A bash command whose stdout is comfortably above the offload preview cap so
# the policy has a reason to fire. Python is used so the output is portable
# across BSD/GNU userlands.
LONG_OUTPUT_CMD = "python3 -c \"print('Z'*1800)\""


# One assistant message with several tool calls. The first is the long bash;
# the rest are tiny echoes. We need enough total messages so the offload
# policy's KEEP_RECENT window does not protect the long one.
SCRIPT_OFFLOAD = [
    {
        "type": "tool_calls",
        "calls": [
            {"id": "b1", "name": "bash", "arguments": {"command": LONG_OUTPUT_CMD}},
            {"id": "b2", "name": "bash", "arguments": {"command": "echo s2"}},
            {"id": "b3", "name": "bash", "arguments": {"command": "echo s3"}},
            {"id": "b4", "name": "bash", "arguments": {"command": "echo s4"}},
            {"id": "b5", "name": "bash", "arguments": {"command": "echo s5"}},
        ],
    },
    {"type": "text", "content": "offload-done"},
]


# Aggressive reclaim: trigger offload at any usage, leave summary inert so
# we are testing the offload path in isolation.
OFFLOAD_ENV = {
    "CONTEXT_WINDOW": "2000",
    "OFFLOAD_THRESHOLD": "0.1",
    "SUMMARY_THRESHOLD": "0.95",
    "MAX_TOKENS": "200",
}


def test_long_tool_output_lands_on_disk(binary: str) -> None:
    """A long bash result must be rewritten to a preview + .agent/offload
    pointer in the next LLM request, and the full content must be on disk."""
    ws = workspace()
    try:
        with MockServer(SCRIPT_OFFLOAD) as mock:
            result = run_agent(
                binary, "go", cwd=ws, extra_env=OFFLOAD_ENV, timeout=10.0
            )
            inspect = mock.inspect()

        assert_exit_ok(result)
        assert_contains(result.stdout, "offload-done")

        # The agent made two requests: one to get the tool call, one to
        # get the final reply. The second request's history must show the
        # offloaded form of the tool message.
        if len(inspect["requests"]) < 2:
            raise AssertionError(
                f"expected ≥2 mock requests, got {len(inspect['requests'])}"
            )
        second = inspect["requests"][1]["messages"]
        tool_msg = find_tool_message(second, "b1")
        body = tool_msg["content"]

        # The full output was 1800 'Z's; the preview should be much shorter.
        if len(body) > 800:
            raise AssertionError(
                f"tool message in second request was not offloaded (length={len(body)})"
            )
        assert_contains(body, "read_file", label="offload hint")
        assert_contains(body, ".agent/offload/", label="offload path")

        # The offload directory should hold the full payload as file 0.
        path = os.path.join(ws, ".agent", "offload", "0.txt")
        if not os.path.isfile(path):
            raise AssertionError(f"offload file missing: {path}")
        with open(path, encoding="utf-8") as fh:
            full = fh.read()
        if "Z" * 1800 not in full:
            raise AssertionError(
                f"offload file does not contain full payload (size={len(full)})"
            )
    finally:
        shutil.rmtree(ws, ignore_errors=True)


def test_offload_recovers_via_read_file(binary: str) -> None:
    """The offload preview points the LLM at read_file. On a follow-up turn
    where the LLM uses that hint, the original payload comes back through
    the existing tool — no new tool needed."""
    ws = workspace()
    try:
        # First turn: produce a long output that gets offloaded.
        # Second turn: the LLM calls read_file with the offload path.
        script = [
            {
                "type": "tool_calls",
                "calls": [
                    {
                        "id": "b1",
                        "name": "bash",
                        "arguments": {"command": LONG_OUTPUT_CMD},
                    },
                    {"id": "b2", "name": "bash", "arguments": {"command": "echo s2"}},
                    {"id": "b3", "name": "bash", "arguments": {"command": "echo s3"}},
                    {"id": "b4", "name": "bash", "arguments": {"command": "echo s4"}},
                    {"id": "b5", "name": "bash", "arguments": {"command": "echo s5"}},
                ],
            },
            {"type": "text", "content": "first-turn-done"},
            {
                "type": "tool_calls",
                "calls": [
                    {
                        "id": "r1",
                        "name": "read_file",
                        "arguments": {"path": ".agent/offload/0.txt"},
                    }
                ],
            },
            {"type": "text", "content": "second-turn-done"},
        ]
        with MockServer(script) as mock:
            result = run_agent(
                binary,
                "make the long thing",
                extra_lines=["now read it back"],
                cwd=ws,
                extra_env=OFFLOAD_ENV,
                timeout=15.0,
            )
            inspect = mock.inspect()

        assert_exit_ok(result)
        # The final request should show the read_file tool message
        # containing the full Z-string.
        last = inspect["requests"][-1]["messages"]
        tm = find_tool_message(last, "r1")
        if "Z" * 1500 not in tm["content"]:
            raise AssertionError(
                f"read_file did not recover the offloaded content "
                f"(length={len(tm['content'])})"
            )
    finally:
        shutil.rmtree(ws, ignore_errors=True)


def test_short_tool_output_is_not_offloaded(binary: str) -> None:
    """Below the preview threshold, the tool message must stay untouched —
    no spurious disk writes, no rewritten body."""
    ws = workspace()
    try:
        script = [
            {
                "type": "tool_calls",
                "calls": [
                    {
                        "id": "b1",
                        "name": "bash",
                        "arguments": {"command": "echo phaseA-short-ok"},
                    }
                ],
            },
            {"type": "text", "content": "short-done"},
        ]
        with MockServer(script) as mock:
            result = run_agent(
                binary, "run a short one", cwd=ws, extra_env=OFFLOAD_ENV, timeout=10.0
            )
            inspect = mock.inspect()

        assert_exit_ok(result)

        if os.path.isdir(os.path.join(ws, ".agent", "offload")):
            files = os.listdir(os.path.join(ws, ".agent", "offload"))
            if files:
                raise AssertionError(
                    f"unexpected offload files for short output: {files}"
                )

        second = inspect["requests"][1]["messages"]
        tm = find_tool_message(second, "b1")
        if "read_file" in tm["content"]:
            raise AssertionError(
                f"short output was wrongly offloaded: {tm['content']!r}"
            )
        assert_contains(
            tm["content"], "phaseA-short-ok", label="short output preserved"
        )
    finally:
        shutil.rmtree(ws, ignore_errors=True)


def get_cases() -> list[TestCase]:
    return [
        TestCase(
            name="long_tool_output_offloaded",
            phase="ca",
            points=14,
            func=test_long_tool_output_lands_on_disk,
        ),
        TestCase(
            name="offload_recovers_via_read_file",
            phase="ca",
            points=12,
            func=test_offload_recovers_via_read_file,
        ),
        TestCase(
            name="short_output_not_offloaded",
            phase="ca",
            points=8,
            func=test_short_tool_output_is_not_offloaded,
        ),
    ]
