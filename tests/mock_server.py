#!/usr/bin/env python3
"""
Mock LLM server for the Project 2 test suite.

Mimics the subset of the OpenAI-style /api/v1/chat/completions endpoint that
the Agent uses. Responses come from a script loaded on startup — the server
pops the next entry on each incoming request, so tests are deterministic.

Script format (JSON array of entries):

  [
    {"type": "text", "content": "done."},
    {"type": "tool_calls", "calls": [
        {"id": "c1", "name": "bash", "arguments": {"command": "echo hi"}}
    ]},
    {"type": "http_status", "status": 500, "body": "boom"}
  ]

Inspection:
  GET /_inspect  -> JSON log of every request the server saw, with the parsed
                    messages array. Tests can assert on history content.
  GET /_health   -> "ok"

Run:
  python3 mock_server.py --script some_script.json [--port 18180]
"""

from __future__ import annotations

import argparse
import json
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from typing import Any, Dict, List

# ── global scripted state ────────────────────────────────────────────────

_lock = threading.Lock()
_script: List[Dict[str, Any]] = []
_cursor: int = 0
_requests: List[Dict[str, Any]] = []  # append-only request log


# ── response builders ────────────────────────────────────────────────────


def _wrap_text(content: str) -> Dict[str, Any]:
    return {
        "id": "mock-response",
        "object": "chat.completion",
        "model": "mock-model",
        "choices": [
            {
                "index": 0,
                "finish_reason": "stop",
                "message": {"role": "assistant", "content": content},
            }
        ],
        "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
    }


def _wrap_tool_calls(calls: List[Dict[str, Any]]) -> Dict[str, Any]:
    """Build an assistant message with tool_calls, matching OpenAI's shape.

    `arguments` must be a *string* (JSON-encoded) in the wire format, even
    though the Agent parses it back into an object.
    """
    wire_calls = []
    for i, c in enumerate(calls):
        args = c.get("arguments", {})
        if not isinstance(args, str):
            args = json.dumps(args)
        wire_calls.append(
            {
                "id": c.get("id", f"call_{i}"),
                "type": "function",
                "function": {"name": c["name"], "arguments": args},
            }
        )
    return {
        "id": "mock-response",
        "object": "chat.completion",
        "model": "mock-model",
        "choices": [
            {
                "index": 0,
                "finish_reason": "tool_calls",
                "message": {
                    "role": "assistant",
                    "content": None,
                    "tool_calls": wire_calls,
                },
            }
        ],
        "usage": {"prompt_tokens": 10, "completion_tokens": 5, "total_tokens": 15},
    }


# ── HTTP handler ─────────────────────────────────────────────────────────


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args: Any) -> None:
        # Silence the default stderr access log — tests run many requests.
        return

    # ── POST /api/v1/chat/completions ──

    def do_POST(self) -> None:  # noqa: N802
        if self.path != "/api/v1/chat/completions":
            self._send_status(404, "not found")
            return

        length = int(self.headers.get("Content-Length", "0"))
        body_bytes = self.rfile.read(length) if length > 0 else b""
        try:
            body = json.loads(body_bytes.decode("utf-8")) if body_bytes else {}
        except json.JSONDecodeError as exc:
            self._send_status(400, f"bad json: {exc}")
            return

        with _lock:
            _requests.append(
                {
                    "path": self.path,
                    "body": body,
                    "messages": body.get("messages", []),
                    "tools": body.get("tools", []),
                    "model": body.get("model"),
                }
            )

            global _cursor
            if _cursor >= len(_script):
                self._send_status(500, "mock script exhausted")
                return

            entry = _script[_cursor]
            _cursor += 1

        kind = entry.get("type", "text")
        if kind == "text":
            self._send_json(200, _wrap_text(entry.get("content", "")))
        elif kind == "tool_calls":
            self._send_json(200, _wrap_tool_calls(entry.get("calls", [])))
        elif kind == "http_status":
            self._send_status(entry.get("status", 500), entry.get("body", ""))
        else:
            self._send_status(500, f"unknown script entry type: {kind!r}")

    # ── GET /_inspect and /_health ──

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/_health":
            self._send_status(200, "ok")
        elif self.path == "/_inspect":
            with _lock:
                payload = {
                    "cursor": _cursor,
                    "script_len": len(_script),
                    "requests": list(_requests),
                }
            self._send_json(200, payload)
        else:
            self._send_status(404, "not found")

    # ── low-level helpers ──

    def _send_json(self, status: int, obj: Dict[str, Any]) -> None:
        body = json.dumps(obj).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _send_status(self, status: int, text: str) -> None:
        body = text.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)


# ── main ─────────────────────────────────────────────────────────────────


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--script", required=True, help="Path to JSON script file")
    ap.add_argument("--port", type=int, default=18180)
    args = ap.parse_args()

    try:
        with open(args.script, "r", encoding="utf-8") as fh:
            global _script
            _script = json.load(fh)
    except OSError as exc:
        print(f"cannot open script: {exc}", file=sys.stderr)
        return 1

    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    # Print a readiness marker on stderr so the test harness can wait for it.
    print(
        f"mock-server: listening on 127.0.0.1:{args.port}", file=sys.stderr, flush=True
    )
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
