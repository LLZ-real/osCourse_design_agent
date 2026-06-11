"""
Test harness for Project 2.
"""

from __future__ import annotations

import contextlib
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request
from dataclasses import dataclass
from typing import Any, Callable, Dict, List, Optional


# ── constants ────────────────────────────────────────────────────────────

DEFAULT_MOCK_PORT = 18180
DEFAULT_TIMEOUT = 10.0  # seconds


# ── MockServer ──────────────────────────────────────────────────────────


class MockServer:
    """Run tests/mock_server.py as a subprocess, cleaned up on exit."""

    def __init__(
        self, script: List[Dict[str, Any]], port: int = DEFAULT_MOCK_PORT
    ) -> None:
        self.script = script
        self.port = port
        self._proc: Optional[subprocess.Popen] = None
        self._script_path: Optional[str] = None

    def __enter__(self) -> "MockServer":
        here = os.path.dirname(os.path.abspath(__file__))
        mock_py = os.path.join(here, "mock_server.py")
        if not os.path.isfile(mock_py):
            raise RuntimeError(f"mock server not found at {mock_py}")

        # Write script to a temp file adjacent to the test directory.
        self._script_path = os.path.join(
            here, f".mock_script_{os.getpid()}_{self.port}.json"
        )
        with open(self._script_path, "w", encoding="utf-8") as fh:
            json.dump(self.script, fh)

        self._proc = subprocess.Popen(
            [
                sys.executable,
                mock_py,
                "--script",
                self._script_path,
                "--port",
                str(self.port),
            ],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.PIPE,
        )
        self._wait_ready()
        return self

    def __exit__(self, *_exc: Any) -> None:
        if self._proc is not None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self._proc.kill()
                self._proc.wait()
            self._proc = None
        if self._script_path and os.path.exists(self._script_path):
            try:
                os.remove(self._script_path)
            except OSError:
                pass

    def _wait_ready(self, timeout: float = 5.0) -> None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with contextlib.suppress(OSError):
                s = socket.create_connection(("127.0.0.1", self.port), timeout=0.3)
                s.close()
                return
            time.sleep(0.05)
        raise RuntimeError(f"mock server never became ready on port {self.port}")

    # ── inspection ──────────────────────────────────────────────────

    def inspect(self) -> Dict[str, Any]:
        with urllib.request.urlopen(
            f"http://127.0.0.1:{self.port}/_inspect", timeout=3
        ) as r:
            return json.loads(r.read().decode("utf-8"))


# ── AgentSession ────────────────────────────────────────────────────────


@dataclass
class AgentResult:
    returncode: int
    stdout: str
    stderr: str


def run_agent(
    binary: str,
    prompt: str,
    *,
    mock_port: int = DEFAULT_MOCK_PORT,
    timeout: float = DEFAULT_TIMEOUT,
    extra_env: Optional[Dict[str, str]] = None,
    extra_lines: Optional[List[str]] = None,
) -> AgentResult:
    """Spawn the agent binary, feed `prompt` (plus any `extra_lines`) on stdin,
    collect stdout/stderr, and return the result.

    `extra_lines` lets Phase C tests submit multiple prompts in one session.
    The harness always finishes with "exit" + EOF so Phase C REPLs terminate.
    """
    env = {
        **os.environ,
        "LLM_HOST": "127.0.0.1",
        "LLM_PORT": str(mock_port),
        "API_KEY": "test-key",
        "MODEL_ID": "mock-model",
        "TERM": "dumb",
    }
    if extra_env:
        env.update(extra_env)

    if extra_lines:
        stdin_text = prompt + "\n" + "\n".join(extra_lines) + "\nexit\n"
    else:
        stdin_text = prompt + "\n"

    try:
        proc = subprocess.run(
            [binary],
            input=stdin_text.encode("utf-8"),
            capture_output=True,
            timeout=timeout,
            env=env,
        )
    except subprocess.TimeoutExpired as exc:
        raise AssertionError(
            f"agent timed out after {timeout}s. stdout={exc.stdout!r} stderr={exc.stderr!r}"
        ) from None

    return AgentResult(
        returncode=proc.returncode,
        stdout=proc.stdout.decode("utf-8", errors="replace"),
        stderr=proc.stderr.decode("utf-8", errors="replace"),
    )


# ── TestCase / runner ───────────────────────────────────────────────────


@dataclass
class TestCase:
    name: str
    phase: str
    points: int
    func: Callable[[str], None]
    visibility: str = "public"


@dataclass
class TestResult:
    case: TestCase
    passed: bool
    message: str = ""


def run_case(case: TestCase, binary: str) -> TestResult:
    try:
        case.func(binary)
        return TestResult(case=case, passed=True)
    except AssertionError as exc:
        return TestResult(case=case, passed=False, message=str(exc))
    except Exception as exc:  # noqa: BLE001
        return TestResult(case=case, passed=False, message=f"Unexpected error: {exc}")


def run_cases(cases: List[TestCase], binary: str) -> List[TestResult]:
    return [run_case(c, binary) for c in cases]


# ── assertion helpers ──────────────────────────────────────────────────


def assert_contains(output: str, expected: str, label: str = "output") -> None:
    if expected not in output:
        raise AssertionError(
            f"{label} does not contain {expected!r}.\n  Got: {output!r}"
        )


def assert_not_contains(output: str, unexpected: str, label: str = "output") -> None:
    if unexpected in output:
        raise AssertionError(
            f"{label} unexpectedly contains {unexpected!r}.\n  Got: {output!r}"
        )


def assert_exit_ok(result: AgentResult) -> None:
    if result.returncode != 0:
        raise AssertionError(
            f"agent exited with code {result.returncode}.\n"
            f"  stderr: {result.stderr!r}\n"
            f"  stdout: {result.stdout!r}"
        )


def assert_exit_nonzero(result: AgentResult) -> None:
    if result.returncode == 0:
        raise AssertionError(
            f"agent unexpectedly exited with code 0.\n"
            f"  stdout: {result.stdout!r}\n"
            f"  stderr: {result.stderr!r}"
        )


def find_tool_message(
    messages: List[Dict[str, Any]], tool_call_id: str
) -> Dict[str, Any]:
    """Return the first message with role=="tool" matching tool_call_id, or raise."""
    for m in messages:
        if m.get("role") == "tool" and m.get("tool_call_id") == tool_call_id:
            return m
    raise AssertionError(
        f"no tool message with tool_call_id={tool_call_id!r} in history.\n"
        f"  messages={messages!r}"
    )
