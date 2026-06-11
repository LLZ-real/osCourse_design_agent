#!/usr/bin/env python3
"""Project 2 test runner."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

from harness import TestCase, TestResult, run_cases  # noqa: E402


def _import_module(path: str):
    name = os.path.splitext(os.path.basename(path))[0]
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


_PHASES = {
    "a": "test_phase_a.py",
    "b": "test_phase_b.py",
    "c": "test_phase_c.py",
}


def collect_cases(phases: list[str]) -> list[TestCase]:
    cases: list[TestCase] = []
    for ph in phases:
        rel = _PHASES[ph]
        path = os.path.join(_HERE, rel)
        if os.path.isfile(path):
            mod = _import_module(path)
            cases.extend(mod.get_cases())
    return cases


_SEP = "\u2501" * 50


def _format_text(results: list[TestResult]) -> str:
    lines: list[str] = []
    for r in results:
        tag = "[PASS]" if r.passed else "[FAIL]"
        label = f"phase_{r.case.phase} :: {r.case.name}"
        pts = f"({r.case.points:>2} pts)"
        lines.append(f"{tag}  {label:<45s} {pts}")
        if not r.passed and r.message:
            for mline in r.message.splitlines():
                lines.append(f"        {mline}")

    lines.append("")
    lines.append(_SEP)

    by_phase: dict[str, tuple[int, int]] = {}
    for r in results:
        earned, total = by_phase.get(r.case.phase, (0, 0))
        by_phase[r.case.phase] = (
            earned + (r.case.points if r.passed else 0),
            total + r.case.points,
        )

    for p in ("a", "b", "c"):
        if p in by_phase:
            earned, total = by_phase[p]
            lines.append(f"Phase {p.upper()}:  {earned:>3} / {total}")

    lines.append(_SEP)
    total_earned = sum(e for e, _ in by_phase.values())
    total_total = sum(t for _, t in by_phase.values())
    lines.append(f"Total:    {total_earned:>3} / {total_total}  (public)")
    lines.append("")
    return "\n".join(lines)


def _format_json(results: list[TestResult]) -> str:
    out = []
    for r in results:
        obj = {
            "phase": r.case.phase,
            "test": r.case.name,
            "points": r.case.points if r.passed else 0,
            "max_points": r.case.points,
            "passed": r.passed,
        }
        if not r.passed and r.message:
            obj["message"] = r.message
        out.append(json.dumps(obj))
    return "\n".join(out)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="./build/c-agent")
    ap.add_argument("--phase", choices=["a", "b", "c"])
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()

    phases = [args.phase] if args.phase else ["a", "b", "c"]
    cases = collect_cases(phases)
    results = run_cases(cases, args.bin)

    if args.json:
        print(_format_json(results))
    else:
        print(_format_text(results))

    sys.exit(1 if any(not r.passed for r in results) else 0)


if __name__ == "__main__":
    main()
