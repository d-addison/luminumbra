#!/usr/bin/env python3
""" doc-lint ().

Assert that docs/shield/sdf-contract.md enumerates the two SDF producer tiers and the
malformed-SDF regeneration rule. The required contract strings are the quoted needles in
test/features/sdf-contract.feature (the `the contract documents "..."` steps), so the BDD
scenario is the single source of truth for what the doc must state.

Exit 0 when every contract string is present; exit 1 (naming the missing strings) otherwise.
Uses only the Python standard library (no numpy/Pillow) so any python3 can run it.
"""
import re
import sys
import pathlib

REPO = pathlib.Path(__file__).resolve().parents[1]
FEATURE = REPO / "test" / "features" / "sdf-contract.feature"
DOC = REPO / "docs" / "shield" / "sdf-contract.md"


def main() -> int:
    for p in (FEATURE, DOC):
        if not p.is_file():
            print(f"SdfContractDocLint: missing required file: {p}", file=sys.stderr)
            return 1

    # Only the Gherkin step lines (Then/And...) carry required needles; prose in the
    # Feature description may mention the step template without asserting anything.
    needles = re.findall(
        r'^\s*(?:Then|And)\s+the contract documents "([^"]+)"',
        FEATURE.read_text(encoding="utf-8"),
        re.MULTILINE,
    )
    if not needles:
        print(
            "SdfContractDocLint: no contract strings found in "
            f"{FEATURE} (expected `the contract documents \"...\"` steps)",
            file=sys.stderr,
        )
        return 1

    text = DOC.read_text(encoding="utf-8")
    missing = [n for n in needles if n not in text]
    if missing:
        print(
            f"SdfContractDocLint: {DOC} is missing required contract strings:",
            file=sys.stderr,
        )
        for m in missing:
            print(f"  - {m!r}", file=sys.stderr)
        return 1

    print(
        f"SdfContractDocLint: OK -- all {len(needles)} contract strings present in "
        f"{DOC.relative_to(REPO)}"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
