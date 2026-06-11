"""Phase B public tests — parallel scheduling for read-only batches."""

from __future__ import annotations

import os
import shutil
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(__file__))

from harness import (
    MockServer,
    TestCase,
    assert_contains,
    assert_exit_ok,
    find_tool_message,
    run_agent,
)


# ── helpers ─────────────────────────────────────────────────────────────


def workspace_with_files(*pairs: tuple[str, str]) -> str:
    d = tempfile.mkdtemp(prefix="agent_ws_")
    for name, content in pairs:
        with open(os.path.join(d, name), "w", encoding="utf-8") as fh:
            fh.write(content)
    return d


# ── Scripts ─────────────────────────────────────────────────────────────

SCRIPT_THREE_PARALLEL_READS = [
    {
        "type": "tool_calls",
        "calls": [
            {"id": "p1", "name": "read_file", "arguments": {"path": "a.txt"}},
            {"id": "p2", "name": "read_file", "arguments": {"path": "b.txt"}},
            {"id": "p3", "name": "read_file", "arguments": {"path": "c.txt"}},
        ],
    },
    {"type": "text", "content": "all-three-read"},
]

SCRIPT_MIXED_READ_WRITE = [
    {
        "type": "tool_calls",
        "calls": [
            {"id": "m1", "name": "read_file", "arguments": {"path": "a.txt"}},
            {
                "id": "m2",
                "name": "write_file",
                "arguments": {"path": "out.txt", "content": "phaseB-mix\n"},
            },
            {"id": "m3", "name": "read_file", "arguments": {"path": "b.txt"}},
        ],
    },
    {"type": "text", "content": "mixed-done"},
]

SCRIPT_MANY_READS = [
    {
        "type": "tool_calls",
        "calls": [
            {"id": f"k{i}", "name": "read_file", "arguments": {"path": f"f{i}.txt"}}
            for i in range(8)
        ],
    },
    {"type": "text", "content": "many-done"},
]


# ── Cases ───────────────────────────────────────────────────────────────


def test_three_parallel_reads_in_order(binary: str) -> None:
    """Three read_file calls in one response — results must appear in id order
    in the next request to the LLM, regardless of completion order.
    """
    ws = workspace_with_files(
        ("a.txt", "alpha-content\n"),
        ("b.txt", "beta-content\n"),
        ("c.txt", "gamma-content\n"),
    )
    try:
        with MockServer(SCRIPT_THREE_PARALLEL_READS) as mock:
            result = run_agent(binary, "read all three", cwd=ws)
            inspect = mock.inspect()

        assert_exit_ok(result)
        assert_contains(result.stdout, "all-three-read")

        second = inspect["requests"][1]["messages"]
        m1 = find_tool_message(second, "p1")
        m2 = find_tool_message(second, "p2")
        m3 = find_tool_message(second, "p3")

        assert_contains(m1["content"], "alpha-content", label="p1")
        assert_contains(m2["content"], "beta-content", label="p2")
        assert_contains(m3["content"], "gamma-content", label="p3")

        # Tool messages must appear in p1, p2, p3 order in history.
        i1 = second.index(m1)
        i2 = second.index(m2)
        i3 = second.index(m3)
        if not (i1 < i2 < i3):
            raise AssertionError(
                f"tool messages out of request order: p1@{i1}, p2@{i2}, p3@{i3}"
            )
    finally:
        shutil.rmtree(ws, ignore_errors=True)


def test_mixed_read_write_falls_back_to_serial(binary: str) -> None:
    """A response mixing read and write must still produce correct results,
    in the right order. The handout's policy says this falls back to serial,
    so the side effects (write_file) must be observable on disk afterwards.
    """
    ws = workspace_with_files(
        ("a.txt", "first\n"),
        ("b.txt", "third\n"),
    )
    try:
        with MockServer(SCRIPT_MIXED_READ_WRITE) as mock:
            result = run_agent(binary, "mix it", cwd=ws)
            inspect = mock.inspect()

        assert_exit_ok(result)
        assert_contains(result.stdout, "mixed-done")

        out = os.path.join(ws, "out.txt")
        if not os.path.isfile(out):
            raise AssertionError("write_file did not create out.txt")
        with open(out, encoding="utf-8") as fh:
            content = fh.read()
        if content != "phaseB-mix\n":
            raise AssertionError(f"out.txt content wrong: {content!r}")

        second = inspect["requests"][1]["messages"]
        m1 = find_tool_message(second, "m1")
        m2 = find_tool_message(second, "m2")
        m3 = find_tool_message(second, "m3")
        assert_contains(m1["content"], "first", label="m1 read")
        assert_contains(m3["content"], "third", label="m3 read")
        if "out.txt" not in m2["content"] and "wrote" not in m2["content"]:
            raise AssertionError(f"m2 write reply looked wrong: {m2['content']!r}")

        i1 = second.index(m1)
        i2 = second.index(m2)
        i3 = second.index(m3)
        if not (i1 < i2 < i3):
            raise AssertionError(
                f"mixed tool messages out of request order: {i1}, {i2}, {i3}"
            )
    finally:
        shutil.rmtree(ws, ignore_errors=True)


def test_eight_parallel_reads_complete(binary: str) -> None:
    """Stress test: dispatch 8 read-only tools in one response. Ensure the
    pool handles a count larger than typical and every result lands. This
    catches "I forgot to wait for one worker" bugs.
    """
    files = [(f"f{i}.txt", f"content-{i}\n") for i in range(8)]
    ws = workspace_with_files(*files)
    try:
        with MockServer(SCRIPT_MANY_READS) as mock:
            result = run_agent(binary, "many reads", cwd=ws)
            inspect = mock.inspect()

        assert_exit_ok(result)
        assert_contains(result.stdout, "many-done")
        second = inspect["requests"][1]["messages"]
        for i in range(8):
            tm = find_tool_message(second, f"k{i}")
            assert_contains(tm["content"], f"content-{i}", label=f"k{i}")
    finally:
        shutil.rmtree(ws, ignore_errors=True)


def test_parallel_speedup(binary: str) -> None:
    """Sanity timing check: 8 parallel read_file calls should not take 8x as
    long as a single one. We use a generous 4x bound so a noisy CI machine
    still passes — the point is to detect "I accidentally serialize even
    when I should parallelize", not to grade absolute throughput.

    Disabled on systems with very fast IO where the per-call overhead
    dominates by setting a long lower bound (50ms) for the single-read
    baseline.
    """
    files = [(f"f{i}.txt", "x" * 32768 + "\n") for i in range(8)]
    ws = workspace_with_files(*files)
    try:
        single_script = [
            {
                "type": "tool_calls",
                "calls": [
                    {
                        "id": "s",
                        "name": "read_file",
                        "arguments": {"path": "f0.txt"},
                    }
                ],
            },
            {"type": "text", "content": "done"},
        ]
        with MockServer(single_script):
            t0 = time.monotonic()
            run_agent(binary, "single", cwd=ws)
            single = time.monotonic() - t0

        with MockServer(SCRIPT_MANY_READS):
            t0 = time.monotonic()
            run_agent(binary, "many", cwd=ws)
            many = time.monotonic() - t0

        # Bail out if the baseline is too small to be a meaningful comparison.
        if single < 0.05:
            return  # IO is too fast for this test to mean anything

        if many > single * 4:
            raise AssertionError(
                f"8 parallel reads took {many:.3f}s vs {single:.3f}s for one — "
                f"that is the kind of slowdown that means scheduling is serial."
            )
    finally:
        shutil.rmtree(ws, ignore_errors=True)


def get_cases() -> list[TestCase]:
    return [
        TestCase(
            name="three_parallel_reads_in_order",
            phase="b",
            points=18,
            func=test_three_parallel_reads_in_order,
        ),
        TestCase(
            name="mixed_read_write_serial_fallback",
            phase="b",
            points=14,
            func=test_mixed_read_write_falls_back_to_serial,
        ),
        TestCase(
            name="eight_parallel_reads_complete",
            phase="b",
            points=12,
            func=test_eight_parallel_reads_complete,
        ),
        TestCase(
            name="parallel_speedup_observed",
            phase="b",
            points=6,
            func=test_parallel_speedup,
        ),
    ]
