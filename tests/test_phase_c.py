"""Phase C public tests."""

from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))

from harness import (
    MockServer,
    TestCase,
    assert_contains,
    assert_exit_ok,
    run_agent,
)


SCRIPT_TWO_TURNS = [
    {"type": "text", "content": "turn-one-reply"},
    {"type": "text", "content": "turn-two-reply"},
]

SCRIPT_SINGLE_TURN = [{"type": "text", "content": "single-ok"}]


def test_multi_turn_history(binary: str) -> None:
    """The second LLM request must contain history from the first turn."""
    with MockServer(SCRIPT_TWO_TURNS) as mock:
        result = run_agent(binary, "first prompt", extra_lines=["second prompt"])
        inspect = mock.inspect()

    assert_exit_ok(result)
    assert_contains(result.stdout, "turn-one-reply")
    assert_contains(result.stdout, "turn-two-reply")

    requests = inspect["requests"]
    if len(requests) != 2:
        raise AssertionError(
            f"expected 2 LLM requests (one per user turn), got {len(requests)}"
        )

    roles_first = [m.get("role") for m in requests[0]["messages"]]
    roles_second = [m.get("role") for m in requests[1]["messages"]]

    if roles_first.count("user") != 1:
        raise AssertionError(
            f"first request should have 1 user message, got roles={roles_first!r}"
        )
    if roles_second.count("user") < 2:
        raise AssertionError(
            f"second request must include both user prompts in history, got roles={roles_second!r}"
        )
    contents = [m.get("content", "") for m in requests[1]["messages"]]
    if not any("first prompt" in (c or "") for c in contents):
        raise AssertionError(
            f"second request missing first user prompt; messages={requests[1]['messages']!r}"
        )


def test_exit_command_terminates_cleanly(binary: str) -> None:
    """`exit` must return from the REPL without hanging the render thread."""
    with MockServer(SCRIPT_SINGLE_TURN):
        result = run_agent(binary, "anything", extra_lines=[], timeout=5.0)
    assert_exit_ok(result)
    assert_contains(result.stdout, "single-ok")


def get_cases() -> list[TestCase]:
    return [
        TestCase(
            name="multi_turn_history",
            phase="c",
            points=24,
            func=test_multi_turn_history,
        ),
        TestCase(
            name="exit_cleanly",
            phase="c",
            points=16,
            func=test_exit_command_terminates_cleanly,
        ),
    ]
