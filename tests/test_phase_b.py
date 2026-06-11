"""Phase B public tests."""

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


SCRIPT_THREE_HOPS = [
    {
        "type": "tool_calls",
        "calls": [
            {"id": "h1", "name": "bash", "arguments": {"command": "echo hop-one"}}
        ],
    },
    {
        "type": "tool_calls",
        "calls": [
            {"id": "h2", "name": "bash", "arguments": {"command": "echo hop-two"}}
        ],
    },
    {
        "type": "tool_calls",
        "calls": [
            {"id": "h3", "name": "bash", "arguments": {"command": "echo hop-three"}}
        ],
    },
    {"type": "text", "content": "loop-done"},
]

SCRIPT_TWO_IN_ONE_RESPONSE = [
    {
        "type": "tool_calls",
        "calls": [
            {"id": "t1", "name": "bash", "arguments": {"command": "echo first"}},
            {"id": "t2", "name": "bash", "arguments": {"command": "echo second"}},
        ],
    },
    {"type": "text", "content": "both-ran"},
]


def test_react_three_hops(binary: str) -> None:
    with MockServer(SCRIPT_THREE_HOPS) as mock:
        result = run_agent(binary, "go")
        inspect = mock.inspect()

    assert_exit_ok(result)
    assert_contains(result.stdout, "loop-done")
    n = len(inspect["requests"])
    if n != 4:
        raise AssertionError(f"expected 4 LLM requests, got {n}")
    final_messages = inspect["requests"][3]["messages"]
    for tid in ("h1", "h2", "h3"):
        find_tool_message(final_messages, tid)


def test_two_tool_calls_in_single_response(binary: str) -> None:
    """Agent must execute both calls and echo both results before next turn."""
    with MockServer(SCRIPT_TWO_IN_ONE_RESPONSE) as mock:
        result = run_agent(binary, "go")
        inspect = mock.inspect()

    assert_exit_ok(result)
    assert_contains(result.stdout, "both-ran")

    second_messages = inspect["requests"][1]["messages"]
    tm1 = find_tool_message(second_messages, "t1")
    tm2 = find_tool_message(second_messages, "t2")
    assert_contains(tm1["content"], "first", label="tool 1 output")
    assert_contains(tm2["content"], "second", label="tool 2 output")

    idx1 = second_messages.index(tm1)
    idx2 = second_messages.index(tm2)
    if idx1 > idx2:
        raise AssertionError(
            f"tool results appear out of order: t1@{idx1} after t2@{idx2}"
        )


def get_cases() -> list[TestCase]:
    return [
        TestCase(
            name="react_three_hops", phase="b", points=22, func=test_react_three_hops
        ),
        TestCase(
            name="two_calls_one_response",
            phase="b",
            points=18,
            func=test_two_tool_calls_in_single_response,
        ),
    ]
