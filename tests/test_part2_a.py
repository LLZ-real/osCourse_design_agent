"""Phase A public tests — tool registry, file tools, sandbox containment."""

from __future__ import annotations

import os
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


# ── helpers ─────────────────────────────────────────────────────────────


def workspace_with_files(*pairs: tuple[str, str]) -> str:
    """Create a temp directory and seed it with files. Returns the directory."""
    d = tempfile.mkdtemp(prefix="agent_ws_")
    for name, content in pairs:
        with open(os.path.join(d, name), "w", encoding="utf-8") as fh:
            fh.write(content)
    return d


# ── Scripts ─────────────────────────────────────────────────────────────

# 1. Bash tool still works through the new dispatch path.
SCRIPT_BASH_HELLO = [
    {
        "type": "tool_calls",
        "calls": [
            {
                "id": "c1",
                "name": "bash",
                "arguments": {"command": "echo phaseA-bash-ok"},
            }
        ],
    },
    {"type": "text", "content": "bash-done"},
]

# 2. read_file
SCRIPT_READ = [
    {
        "type": "tool_calls",
        "calls": [
            {"id": "r1", "name": "read_file", "arguments": {"path": "hello.txt"}}
        ],
    },
    {"type": "text", "content": "read-done"},
]

# 3. write_file
SCRIPT_WRITE = [
    {
        "type": "tool_calls",
        "calls": [
            {
                "id": "w1",
                "name": "write_file",
                "arguments": {"path": "out.txt", "content": "phaseA-write-payload\n"},
            }
        ],
    },
    {"type": "text", "content": "write-done"},
]

# 4. edit_file
SCRIPT_EDIT = [
    {
        "type": "tool_calls",
        "calls": [
            {
                "id": "e1",
                "name": "edit_file",
                "arguments": {
                    "path": "hello.txt",
                    "old_text": "world",
                    "new_text": "agent",
                },
            }
        ],
    },
    {"type": "text", "content": "edit-done"},
]

# 5. sandbox escape via ".." (file tool must reject)
SCRIPT_READ_ESCAPE = [
    {
        "type": "tool_calls",
        "calls": [
            {
                "id": "x1",
                "name": "read_file",
                "arguments": {"path": "../etc/passwd"},
            }
        ],
    },
    {"type": "text", "content": "escape-handled"},
]

# 6. Unknown tool: dispatch must fall through registry, not crash
SCRIPT_UNKNOWN = [
    {
        "type": "tool_calls",
        "calls": [{"id": "u1", "name": "definitely_not_a_tool", "arguments": {}}],
    },
    {"type": "text", "content": "unknown-handled"},
]


# ── Cases ───────────────────────────────────────────────────────────────


def test_tools_advertised(binary: str) -> None:
    """Every registered tool must appear on the wire to the LLM."""
    with MockServer([{"type": "text", "content": "noop"}]) as mock:
        run_agent(binary, "hi")
        inspect = mock.inspect()

    tools = inspect["requests"][0]["tools"]
    names = sorted(t["function"]["name"] for t in tools)
    expected = sorted(["bash", "read_file", "write_file", "edit_file"])
    if names != expected:
        raise AssertionError(
            f"tool list does not match the registry. expected {expected}, got {names}"
        )


def test_bash_still_works(binary: str) -> None:
    """The bash tool still runs end-to-end through the registry."""
    with MockServer(SCRIPT_BASH_HELLO) as mock:
        result = run_agent(binary, "run echo")
        inspect = mock.inspect()

    assert_exit_ok(result)
    assert_contains(result.stdout, "bash-done")
    second = inspect["requests"][1]["messages"]
    tool_msg = find_tool_message(second, "c1")
    assert_contains(tool_msg["content"], "phaseA-bash-ok", label="bash output")


def test_read_file(binary: str) -> None:
    """read_file returns workspace-relative file content to the LLM."""
    ws = workspace_with_files(("hello.txt", "hello world\n"))
    try:
        with MockServer(SCRIPT_READ) as mock:
            result = run_agent(binary, "read it", extra_env={"WS_DIR": ws}, cwd=ws)
            inspect = mock.inspect()
    finally:
        import shutil

        shutil.rmtree(ws, ignore_errors=True)

    assert_exit_ok(result)
    second = inspect["requests"][1]["messages"]
    tool_msg = find_tool_message(second, "r1")
    assert_contains(tool_msg["content"], "hello world", label="read_file payload")


def test_write_file(binary: str) -> None:
    """write_file creates the file with the given content."""
    ws = workspace_with_files()
    try:
        with MockServer(SCRIPT_WRITE) as mock:
            result = run_agent(binary, "write it", cwd=ws)
            inspect = mock.inspect()

        assert_exit_ok(result)
        target = os.path.join(ws, "out.txt")
        if not os.path.isfile(target):
            raise AssertionError(f"write_file did not create {target}")
        with open(target, "r", encoding="utf-8") as fh:
            text = fh.read()
        if text != "phaseA-write-payload\n":
            raise AssertionError(f"file content mismatch: {text!r}")

        second = inspect["requests"][1]["messages"]
        tool_msg = find_tool_message(second, "w1")
        # The tool message should mention bytes written or path.
        if "out.txt" not in tool_msg["content"] and "wrote" not in tool_msg["content"]:
            raise AssertionError(
                f"write_file tool message looked wrong: {tool_msg['content']!r}"
            )
    finally:
        import shutil

        shutil.rmtree(ws, ignore_errors=True)


def test_edit_file(binary: str) -> None:
    """edit_file replaces exact text."""
    ws = workspace_with_files(("hello.txt", "hello world!\n"))
    try:
        with MockServer(SCRIPT_EDIT) as mock:
            result = run_agent(binary, "edit it", cwd=ws)
            inspect = mock.inspect()

        assert_exit_ok(result)
        with open(os.path.join(ws, "hello.txt"), "r", encoding="utf-8") as fh:
            text = fh.read()
        if text != "hello agent!\n":
            raise AssertionError(f"edit_file produced wrong content: {text!r}")

        tool_msg = find_tool_message(inspect["requests"][1]["messages"], "e1")
        if (
            "updated" not in tool_msg["content"]
            and "hello.txt" not in tool_msg["content"]
        ):
            raise AssertionError(
                f"edit_file tool message looked wrong: {tool_msg['content']!r}"
            )
    finally:
        import shutil

        shutil.rmtree(ws, ignore_errors=True)


def test_read_escape_workspace(binary: str) -> None:
    """Sandbox must reject paths that escape the workspace."""
    ws = workspace_with_files()
    try:
        with MockServer(SCRIPT_READ_ESCAPE) as mock:
            result = run_agent(binary, "try to escape", cwd=ws)
            inspect = mock.inspect()

        assert_exit_ok(result)
        tool_msg = find_tool_message(inspect["requests"][1]["messages"], "x1")
        msg = tool_msg["content"].lower()
        if "escape" not in msg and "workspace" not in msg and "outside" not in msg:
            raise AssertionError(
                f"sandbox rejection message looks wrong: {msg!r}. "
                f"It must clearly say the path was rejected."
            )
    finally:
        import shutil

        shutil.rmtree(ws, ignore_errors=True)


def test_unknown_tool_via_registry(binary: str) -> None:
    """A tool name not in the registry must be reported, not crash."""
    with MockServer(SCRIPT_UNKNOWN) as mock:
        result = run_agent(binary, "make up a tool")
        inspect = mock.inspect()

    assert_exit_ok(result)
    tool_msg = find_tool_message(inspect["requests"][1]["messages"], "u1")
    msg = tool_msg["content"].lower()
    if "unknown" not in msg and "definitely_not_a_tool" not in msg:
        raise AssertionError(f"unknown tool report looks wrong: {msg!r}")


def get_cases() -> list[TestCase]:
    return [
        TestCase(
            name="tools_advertised",
            phase="a",
            points=10,
            func=test_tools_advertised,
        ),
        TestCase(
            name="bash_through_registry",
            phase="a",
            points=8,
            func=test_bash_still_works,
        ),
        TestCase(
            name="read_file",
            phase="a",
            points=12,
            func=test_read_file,
        ),
        TestCase(
            name="write_file",
            phase="a",
            points=12,
            func=test_write_file,
        ),
        TestCase(
            name="edit_file",
            phase="a",
            points=12,
            func=test_edit_file,
        ),
        TestCase(
            name="sandbox_rejects_escape",
            phase="a",
            points=10,
            func=test_read_escape_workspace,
        ),
        TestCase(
            name="unknown_tool_via_registry",
            phase="a",
            points=6,
            func=test_unknown_tool_via_registry,
        ),
    ]
