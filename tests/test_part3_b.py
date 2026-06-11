from __future__ import annotations

import os
import sys

sys.path.insert(0, os.path.dirname(__file__))

from harness import (
    MockServer,
    TestCase,
    assert_exit_ok,
    run_agent,
)


# Plenty of generic responses. The mock script is consumed by both the
# agent's main calls and the summary policy's recursive calls.
TEXT_RESPONSES = [{"type": "text", "content": f"ack-{i}"} for i in range(40)]


# Reclaim policy: only summary should fire. The window stays generous so
# the failure mode "I exceeded the window" cannot be confused with
# "summary did not fire".
SUMMARY_ENV = {
    "CONTEXT_WINDOW": "500",
    "OFFLOAD_THRESHOLD": "0.95",
    "SUMMARY_THRESHOLD": "0.3",
    "MAX_TOKENS": "200",
}


def _is_summary_request(req: dict, typed_turns: set[str]) -> bool:
    """A summary call uses a throwaway one-message history, not the live chat."""
    messages = [m for m in req.get("messages", []) if m.get("role") != "system"]
    if len(messages) != 1 or messages[0].get("role") != "user":
        return False
    return (messages[0].get("content") or "") not in typed_turns


def test_summary_fires_recursive_llm_call(binary: str) -> None:
    """Multi-turn dialogue under pressure must produce at least one
    distinguishable summary request — the proof that the runtime recursed
    into the LLM to compress its own history."""
    turns = ["t%d" % i for i in range(10)]

    with MockServer(TEXT_RESPONSES) as mock:
        result = run_agent(
            binary,
            turns[0],
            extra_lines=turns[1:],
            extra_env=SUMMARY_ENV,
            timeout=20.0,
        )
        inspect = mock.inspect()

    assert_exit_ok(result)

    typed_turns = set(turns)
    summary_requests = [
        r for r in inspect["requests"] if _is_summary_request(r, typed_turns)
    ]
    if not summary_requests:
        raise AssertionError(
            f"no summary request observed across {len(inspect['requests'])} mock calls; "
            "the summary policy did not call llm_chat"
        )


def test_summary_reduces_message_count(binary: str) -> None:
    """After summary fires, the next agent main request must carry strictly
    fewer messages than the cumulative count of user prompts typed up to
    that point — proof that summary collapsed history rather than appending."""
    turns = ["t%d" % i for i in range(10)]

    with MockServer(TEXT_RESPONSES) as mock:
        result = run_agent(
            binary,
            turns[0],
            extra_lines=turns[1:],
            extra_env=SUMMARY_ENV,
            timeout=20.0,
        )
        inspect = mock.inspect()

    assert_exit_ok(result)

    # Find the index of the first summary request, then look at the next
    # main (non-summary) request. Its user-message count must be < the
    # number of user prompts typed by then.
    requests = inspect["requests"]
    typed_turns = set(turns)
    main_request_indices = [
        i for i, r in enumerate(requests) if not _is_summary_request(r, typed_turns)
    ]
    summary_idxs = [
        i for i, r in enumerate(requests) if _is_summary_request(r, typed_turns)
    ]
    if not summary_idxs:
        raise AssertionError("no summary request observed")

    first_summary = summary_idxs[0]
    # Cumulative user-prompts typed by the time the first summary fired
    # equals the number of main requests up to and including the one whose
    # post-reclaim state triggered summary.
    prior_main_calls = sum(1 for i in main_request_indices if i < first_summary)
    typed_so_far = prior_main_calls + 1  # the user prompt currently being processed

    # Find the next main request after the summary call.
    next_main = next((i for i in main_request_indices if i > first_summary), None)
    if next_main is None:
        raise AssertionError("no main request after the first summary call")

    msgs = requests[next_main]["messages"]
    user_count = sum(1 for m in msgs if m.get("role") == "user")

    if user_count >= typed_so_far + 1:
        # Allow a small slack: the user-role count includes the synthetic
        # summary message itself. The real check is that user_count is
        # much smaller than the natural growth of typed_so_far.
        raise AssertionError(
            f"after summary, request still has {user_count} user messages "
            f"but only {typed_so_far} prompts had been typed — summary did "
            f"not actually collapse history"
        )


def get_cases() -> list[TestCase]:
    return [
        TestCase(
            name="summary_recursive_llm_call",
            phase="cb",
            points=14,
            func=test_summary_fires_recursive_llm_call,
        ),
        TestCase(
            name="summary_reduces_message_count",
            phase="cb",
            points=12,
            func=test_summary_reduces_message_count,
        ),
    ]
