#!/usr/bin/env python3
"""Bless a candidate capture as the new golden reference for visual regression.

A thin, auditable wrapper over flip_diff.bless_golden so "update the golden" is
its own explicit, greppable command in CI logs and history (distinct from a
normal diff run). `flip_diff.py --update <cand> <golden>` does the same thing;
this script exists so blessing a golden is a deliberate, named action.

By default it refuses to overwrite an existing golden unless --force is given,
so a golden is never re-blessed by accident (a regression must be reviewed by a
human before its NEW look becomes the reference). Pass --diff to print the
perceptual delta between the old golden and the candidate before blessing, so
you see exactly how much you are moving the reference.

Usage:
  python tools/golden_update.py <candidate> <golden> [--force] [--diff]

Exit codes: 0 = blessed, 1 = refused (golden exists, no --force), 2 = I/O error.
"""
import sys
import os
import argparse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flip_diff  # noqa: E402


def main(argv):
    p = argparse.ArgumentParser(
        prog="golden_update.py",
        description="Bless a candidate image as the new golden reference.")
    p.add_argument("candidate", help="candidate image to promote (PNG/PPM)")
    p.add_argument("golden", help="destination golden path")
    p.add_argument("--force", action="store_true",
                   help="overwrite an existing golden (required if it exists)")
    p.add_argument("--diff", action="store_true",
                   help="print the perceptual delta old-golden vs candidate first")
    args = p.parse_args(argv[1:])

    if not os.path.isfile(args.candidate):
        print(f"error: candidate not found: {args.candidate}", file=sys.stderr)
        return 2

    exists = os.path.isfile(args.golden)
    if exists and args.diff:
        try:
            r = flip_diff.compare(args.candidate, args.golden, backend="auto")
            print(f"[golden_update] delta old-golden vs candidate: "
                  f"backend={r['backend']} score={r['score']:.6f}")
        except Exception as e:
            print(f"[golden_update] (diff skipped: {e})", file=sys.stderr)

    if exists and not args.force:
        print(f"error: golden already exists: {args.golden}\n"
              f"       re-run with --force to overwrite (review the change first).",
              file=sys.stderr)
        return 1

    try:
        dst = flip_diff.bless_golden(args.candidate, args.golden)
    except Exception as e:
        print(f"error: bless failed: {e}", file=sys.stderr)
        return 2
    action = "RE-BLESSED" if exists else "BLESSED"
    print(f"{action} {args.candidate} -> golden {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
