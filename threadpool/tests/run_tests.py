#!/usr/bin/env python3
"""
Test runner for lab3-threadpool.

Executes C test binaries, parses [PASS]/[FAIL] output, computes per-phase
and total scores, and prints a unified report.

Usage:
    python3 tests/run_tests.py [--timeout SECS] [--phase {a,b,c,d}] [--json] [--hidden]
"""

import argparse
import json
import os
import re
import signal
import subprocess
import sys

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

PHASES = [
    {"name": "a", "binary": "test_phase_a", "points": 10, "label": "Phase A"},
    {"name": "b", "binary": "test_phase_b", "points": 30, "label": "Phase B"},
    {"name": "c", "binary": "test_phase_c", "points": 30, "label": "Phase C"},
    {"name": "d", "binary": "test_phase_d", "points": 30, "label": "Phase D"},
]

HIDDEN_PHASE = {
    "name": "hidden",
    "binary": "test_hidden",
    "points": 30,
    "label": "Hidden",
}

DEFAULT_TIMEOUT = 5

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

_RESULT_RE = re.compile(r"^\s*\[(PASS|FAIL)\]\s+(.+)$")


def _signal_name(signum):
    """Return a human-readable signal name, e.g. 'SIGSEGV (11)'."""
    try:
        name = signal.Signals(signum).name
    except (ValueError, AttributeError):
        name = "signal"
    return f"{name} ({signum})"


def run_binary(binary_path, timeout):
    """Run a test binary and return (stdout, stderr, exit_code, error_msg).

    *error_msg* is None on normal completion, or a short description if the
    binary timed out or was killed by a signal.
    """
    try:
        proc = subprocess.run(
            [binary_path],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=timeout,
        )
        stdout = proc.stdout.decode("utf-8", errors="replace")
        stderr = proc.stderr.decode("utf-8", errors="replace")
        exit_code = proc.returncode

        # On UNIX a negative returncode means killed by signal.
        if exit_code < 0:
            signum = -exit_code
            return stdout, stderr, exit_code, f"killed by {_signal_name(signum)}"

        return stdout, stderr, exit_code, None

    except subprocess.TimeoutExpired:
        return "", "", -1, f"timeout after {timeout} seconds"
    except FileNotFoundError:
        return "", "", -1, f"binary not found: {binary_path}"
    except OSError as exc:
        return "", "", -1, str(exc)


def parse_results(stdout):
    """Parse [PASS]/[FAIL] lines from test output.

    Returns a list of (passed: bool, test_name: str, reason: str|None).
    """
    results = []
    for line in stdout.splitlines():
        m = _RESULT_RE.match(line)
        if not m:
            continue
        status = m.group(1)
        rest = m.group(2).strip()
        # A FAIL line may contain " -- reason"
        if status == "FAIL" and " -- " in rest:
            name, reason = rest.split(" -- ", 1)
            results.append((False, name.strip(), reason.strip()))
        else:
            results.append((status == "PASS", rest, None))
    return results


def distribute_points(total_points, num_tests):
    """Distribute *total_points* among *num_tests* as evenly as possible.

    Returns a list of length *num_tests* that sums to *total_points*.
    Each element is an integer >= 0.
    """
    if num_tests <= 0:
        return []
    base = total_points // num_tests
    remainder = total_points % num_tests
    pts = [base] * num_tests
    # Give the extra points to the last tests so the first tests show the
    # 'base' value -- keeps the output tidy.
    for i in range(num_tests - remainder, num_tests):
        pts[i] += 1
    return pts


# ---------------------------------------------------------------------------
# Running a single phase
# ---------------------------------------------------------------------------


def run_phase(phase, root_dir, timeout):
    """Run one phase and return a list of test-result dicts.

    Each dict:
        {
            "phase":  "phase_a",
            "test":   "test name",
            "passed": bool,
            "points": int,         # points earned
            "max":    int,         # points possible
            "reason": str | None,
        }
    """
    binary = os.path.join(root_dir, phase["binary"])
    phase_label = f"phase_{phase['name']}"

    stdout, _stderr, _exit_code, error_msg = run_binary(binary, timeout)

    if error_msg:
        # We don't know how many tests exist when the binary can't run.
        # Return a single synthetic failure covering all points.
        return [
            {
                "phase": phase_label,
                "test": "(all tests)",
                "passed": False,
                "points": 0,
                "max": phase["points"],
                "reason": error_msg,
            }
        ]

    parsed = parse_results(stdout)

    if not parsed:
        # Binary ran but produced no recognisable output.
        return [
            {
                "phase": phase_label,
                "test": "(no output)",
                "passed": False,
                "points": 0,
                "max": phase["points"],
                "reason": "binary produced no test results",
            }
        ]

    pts = distribute_points(phase["points"], len(parsed))
    results = []
    for (passed, name, reason), max_pts in zip(parsed, pts):
        results.append(
            {
                "phase": phase_label,
                "test": name,
                "passed": passed,
                "points": max_pts if passed else 0,
                "max": max_pts,
                "reason": reason,
            }
        )
    return results


# ---------------------------------------------------------------------------
# Output formatting
# ---------------------------------------------------------------------------

_WIDTH = 60


def _format_pts(passed, earned, maximum):
    """Format the points column consistently."""
    if passed:
        return f"({earned:>2} pts)"
    else:
        return f"( 0/{maximum:<2})"


def print_results(all_results, phase_order, show_hidden):
    """Pretty-print the unified test report."""
    for r in all_results:
        tag = "[PASS]" if r["passed"] else "[FAIL]"
        label = f"{r['phase']} :: {r['test']}"
        pts = _format_pts(r["passed"], r["points"], r["max"])
        reason = ""
        if not r["passed"] and r["reason"]:
            reason = f"  -- {r['reason']}"
        # Truncate label if it would overflow
        max_label = _WIDTH - len(tag) - len(pts) - 4
        if len(label) > max_label:
            label = label[: max_label - 1] + "\u2026"
        print(f"{tag}  {label:<{max_label}}  {pts}{reason}")

    # Separator
    print("\u2501" * _WIDTH)

    # Per-phase summary
    public_earned = 0
    public_max = 0
    hidden_earned = 0
    hidden_max = 0

    for pname in phase_order:
        phase_results = [r for r in all_results if r["phase"] == pname]
        earned = sum(r["points"] for r in phase_results)
        maximum = sum(r["max"] for r in phase_results)
        # Nicer capitalisation
        if pname == "phase_hidden":
            display = "Hidden"
        elif pname.startswith("phase_"):
            letter = pname[-1].upper()
            display = f"Phase {letter}"
        else:
            display = pname
        print(f"{display}:  {earned:>3} / {maximum}")
        if pname == "phase_hidden":
            hidden_earned += earned
            hidden_max += maximum
        else:
            public_earned += earned
            public_max += maximum

    print("\u2501" * _WIDTH)

    if show_hidden:
        print(
            f"Total:    {public_earned + hidden_earned:>3} / {public_max + hidden_max}  (public + hidden)"
        )
    else:
        print(f"Total:    {public_earned:>3} / {public_max}  (public)")


def print_json(all_results, phase_order, show_hidden):
    """Output results as JSON Lines."""
    for r in all_results:
        print(json.dumps(r))

    # Summary line
    public_earned = 0
    public_max = 0
    hidden_earned = 0
    hidden_max = 0

    for pname in phase_order:
        phase_results = [r for r in all_results if r["phase"] == pname]
        earned = sum(r["points"] for r in phase_results)
        maximum = sum(r["max"] for r in phase_results)
        if pname == "phase_hidden":
            hidden_earned += earned
            hidden_max += maximum
        else:
            public_earned += earned
            public_max += maximum

    summary = {
        "summary": True,
        "public_earned": public_earned,
        "public_max": public_max,
    }
    if show_hidden:
        summary["hidden_earned"] = hidden_earned
        summary["hidden_max"] = hidden_max
        summary["total_earned"] = public_earned + hidden_earned
        summary["total_max"] = public_max + hidden_max
    else:
        summary["total_earned"] = public_earned
        summary["total_max"] = public_max
    print(json.dumps(summary))


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main():
    parser = argparse.ArgumentParser(description="Thread pool test runner")
    parser.add_argument(
        "--timeout",
        type=int,
        default=DEFAULT_TIMEOUT,
        help=f"Per-binary timeout in seconds (default: {DEFAULT_TIMEOUT})",
    )
    parser.add_argument(
        "--phase",
        choices=["a", "b", "c", "d"],
        default=None,
        help="Run only this phase",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Output JSON Lines instead of pretty text",
    )
    parser.add_argument(
        "--hidden",
        action="store_true",
        help="Also run hidden tests",
    )
    args = parser.parse_args()

    # Resolve project root (one directory up from tests/)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    root_dir = os.path.dirname(script_dir)

    # Decide which phases to run
    if args.phase:
        selected = [p for p in PHASES if p["name"] == args.phase]
    else:
        selected = list(PHASES)

    if args.hidden:
        selected.append(HIDDEN_PHASE)

    # Run them
    all_results = []
    phase_order = []
    for phase in selected:
        phase_label = f"phase_{phase['name']}"
        phase_order.append(phase_label)
        results = run_phase(phase, root_dir, args.timeout)
        all_results.extend(results)

    # Output
    if args.json:
        print_json(all_results, phase_order, args.hidden)
    else:
        print_results(all_results, phase_order, args.hidden)

    # Exit code: 0 if all passed, 1 otherwise
    all_passed = all(r["passed"] for r in all_results)
    sys.exit(0 if all_passed else 1)


if __name__ == "__main__":
    main()
