"""Phase A public tests."""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))

from harness import (
    MockServer,
    TestCase,
    assert_contains,
    assert_exit_ok,
    find_tool_message,
    run_agent,
)


# ── Scripts ──────────────────────────────────────────────────────────────

SCRIPT_BASH_ECHO = [
    {
        "type": "tool_calls",
        "calls": [
            {
                "id": "c1",
                "name": "bash",
                "arguments": {"command": "echo agent-says-hello"},
            }
        ],
    },
    {"type": "text", "content": "done-final"},
]

SCRIPT_BASH_FALSE = [
    {
        "type": "tool_calls",
        "calls": [{"id": "c1", "name": "bash", "arguments": {"command": "false"}}],
    },
    {"type": "text", "content": "i-handled-the-error"},
]

SCRIPT_UNKNOWN_TOOL = [
    {
        "type": "tool_calls",
        "calls": [{"id": "c1", "name": "nope", "arguments": {"foo": 1}}],
    },
    {"type": "text", "content": "moved-on"},
]

SCRIPT_TEXT_ONLY = [{"type": "text", "content": "hello-world"}]

SCRIPT_HTTP_500 = [{"type": "http_status", "status": 500, "body": "boom"}]


# ── Cases ───────────────────────────────────────────────────────────────


def test_text_only_reply(binary: str) -> None:
    """No tool call in response: agent should print the text and exit 0."""
    with MockServer(SCRIPT_TEXT_ONLY):
        result = run_agent(binary, "hi there")
    assert_exit_ok(result)
    assert_contains(result.stdout, "hello-world")


def test_bash_tool_call_and_final_reply(binary: str) -> None:
    """Full loop: tool_call -> bash runs -> result goes back -> final text."""
    with MockServer(SCRIPT_BASH_ECHO) as mock:
        result = run_agent(binary, "run echo")
        inspect = mock.inspect()

    assert_exit_ok(result)
    assert_contains(result.stdout, "done-final")

    requests = inspect["requests"]
    if len(requests) < 2:
        raise AssertionError(
            f"expected >=2 LLM requests (tool-call + final), got {len(requests)}: {requests!r}"
        )
    second_messages = requests[1]["messages"]
    tool_msg = find_tool_message(second_messages, "c1")
    assert_contains(
        tool_msg.get("content", ""),
        "agent-says-hello",
        label="tool result echoed to LLM",
    )


def test_bash_failure_captured(binary: str) -> None:
    """A failing bash command must be reported back to the LLM, not abort."""
    with MockServer(SCRIPT_BASH_FALSE) as mock:
        result = run_agent(binary, "run false")
        inspect = mock.inspect()

    assert_exit_ok(result)
    assert_contains(result.stdout, "i-handled-the-error")
    second_messages = inspect["requests"][1]["messages"]
    tool_msg = find_tool_message(second_messages, "c1")
    assert_contains(tool_msg["content"], "exit", label="bash error prefix")


def test_unknown_tool_does_not_crash(binary: str) -> None:
    """If the LLM names a tool we don't have, report it cleanly and continue."""
    with MockServer(SCRIPT_UNKNOWN_TOOL) as mock:
        result = run_agent(binary, "run nope")
        inspect = mock.inspect()

    assert_exit_ok(result)
    assert_contains(result.stdout, "moved-on")
    second_messages = inspect["requests"][1]["messages"]
    tool_msg = find_tool_message(second_messages, "c1")
    msg = tool_msg["content"].lower()
    if "unknown" not in msg and "nope" not in msg:
        raise AssertionError(
            f"expected tool message to mention unknown/nope, got {msg!r}"
        )


def test_http_500_surfaces_error(binary: str) -> None:
    """HTTP 5xx from the LLM must be surfaced, not silently treated as success.

    We accept either a non-zero exit (Phase A single-shot contract) or a
    zero exit where the error shows up in stderr/stdout (the Phase C REPL
    recovers and keeps running).
    """
    with MockServer(SCRIPT_HTTP_500):
        result = run_agent(binary, "whatever")

    combined = (result.stdout + "\n" + result.stderr).lower()
    surfaced = ("500" in combined) or ("error" in combined)
    if result.returncode == 0 and not surfaced:
        raise AssertionError(
            "agent exited 0 but did not surface the HTTP error anywhere.\n"
            f"  stdout: {result.stdout!r}\n  stderr: {result.stderr!r}"
        )


def get_cases() -> list[TestCase]:
    return [
        TestCase(
            name="text_only_reply", phase="a", points=10, func=test_text_only_reply
        ),
        TestCase(
            name="bash_tool_call_flow",
            phase="a",
            points=18,
            func=test_bash_tool_call_and_final_reply,
        ),
        TestCase(
            name="bash_failure_captured",
            phase="a",
            points=14,
            func=test_bash_failure_captured,
        ),
        TestCase(
            name="unknown_tool_graceful",
            phase="a",
            points=10,
            func=test_unknown_tool_does_not_crash,
        ),
        TestCase(
            name="http_500_surfaces_error",
            phase="a",
            points=8,
            func=test_http_500_surfaces_error,
        ),
    ]
