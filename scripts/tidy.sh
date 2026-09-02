#!/usr/bin/env bash
set -euo pipefail

# Blocking clang-tidy gate over first-party engine sources.
#
# This is intentionally a small, curated subset of checks that the current
# tree passes 100% clean, so the gate can block regressions from day one.
# The advisory superset stays in .clang-tidy for local/editor use; this
# script owns the CI-blocking configuration.
#
# Usage:
#   scripts/tidy.sh                       # analyze every first-party TU
#   scripts/tidy.sh --changed-from REF    # analyze TUs changed since REF
#   scripts/tidy.sh --build-dir DIR       # compile db location (default build/tidy)
#   scripts/tidy.sh --jobs N              # parallel clang-tidy processes

build_dir="build/tidy"
changed_from=""
jobs="$(nproc)"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            build_dir="$2"
            shift 2
            ;;
        --changed-from)
            changed_from="$2"
            shift 2
            ;;
        --jobs)
            jobs="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

# Curated blocking subset. Every check listed here is clean tree-wide today;
# add a check only after verifying a full-tree run passes (or fixing the
# violations first). readability-function-cognitive-complexity grandfathers
# existing large functions via the threshold while capping further growth.
gate_checks="-*"
gate_checks+=",bugprone-dangling-handle"
gate_checks+=",bugprone-infinite-loop"
gate_checks+=",bugprone-unchecked-optional-access"
gate_checks+=",bugprone-use-after-move"
gate_checks+=",clang-analyzer-core*"
gate_checks+=",modernize-use-nullptr"
gate_checks+=",performance-for-range-copy"
gate_checks+=",performance-move-const-arg"
gate_checks+=",readability-function-cognitive-complexity"

# Threshold 250 is the smallest round value that grandfathers every existing
# function (current worst first-party function scores 243; main_client.cpp's
# main() carries an explicit NOLINT and is tracked for decomposition).
gate_config='{
  "Checks": "'"$gate_checks"'",
  "WarningsAsErrors": "*",
  "HeaderFilterRegex": "",
  "FormatStyle": "file",
  "CheckOptions": [
    {"key": "readability-function-cognitive-complexity.Threshold", "value": "250"},
    {"key": "modernize-use-nullptr.NullMacros", "value": "NULL"}
  ]
}'

clang_tidy=""
for candidate in clang-tidy-18 clang-tidy; do
    if command -v "$candidate" >/dev/null 2>&1; then
        clang_tidy="$candidate"
        break
    fi
done
if [[ -z "$clang_tidy" ]]; then
    echo "clang-tidy was not found on PATH." >&2
    exit 1
fi

compile_db="$repo_root/$build_dir/compile_commands.json"
if [[ ! -f "$compile_db" ]]; then
    echo "clang-tidy requires $compile_db. Configure CMake with compile commands enabled first." >&2
    echo "Example: cmake -S . -B $build_dir -G Ninja -DCMAKE_BUILD_TYPE=Release \\" >&2
    echo "  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DLUMINUMBRA_WARNINGS_AS_ERRORS=OFF \\" >&2
    echo "  -DCMAKE_C_COMPILER=clang-18 -DCMAKE_CXX_COMPILER=clang++-18" >&2
    exit 1
fi

# Only translation units present in the compile database can be analyzed;
# TUs dormant in this configuration (e.g. optional backends) are skipped.
declare -A compile_db_files=()
while IFS= read -r -d '' db_path; do
    compile_db_files["$db_path"]=1
done < <(python3 -c '
import json
import sys

for entry in json.load(open(sys.argv[1])):
    sys.stdout.write(entry["file"] + "\0")
' "$compile_db")

# First-party translation units live under src/. The RmlUi GL3 backend is
# vendored upstream code carried in-tree and is excluded from the gate.
is_first_party_tu() {
    case "$1" in
        src/luminumbra_client/ui/gl3/*)
            return 1
            ;;
        src/*.c|src/*.cc|src/*.cpp|src/*.cxx)
            return 0
            ;;
    esac
    return 1
}

tidy_files=()
if [[ -n "$changed_from" ]]; then
    if ! git -C "$repo_root" rev-parse --verify --quiet "$changed_from^{commit}" >/dev/null; then
        echo "Tidy comparison commit does not exist: $changed_from" >&2
        exit 1
    fi

    while IFS= read -r -d '' relative_path; do
        if is_first_party_tu "$relative_path" &&
            [[ -n "${compile_db_files[$repo_root/$relative_path]:-}" ]]; then
            tidy_files+=("$repo_root/$relative_path")
        fi
    done < <(git -C "$repo_root" diff --name-only --diff-filter=ACMR -z "$changed_from" -- src)

    while IFS= read -r -d '' relative_path; do
        if is_first_party_tu "$relative_path" &&
            [[ -n "${compile_db_files[$repo_root/$relative_path]:-}" ]]; then
            tidy_files+=("$repo_root/$relative_path")
        fi
    done < <(git -C "$repo_root" ls-files --others --exclude-standard -z -- src)
else
    while IFS= read -r -d '' absolute_path; do
        if is_first_party_tu "${absolute_path#"$repo_root/"}"; then
            tidy_files+=("$absolute_path")
        fi
    done < <(printf '%s\0' "${!compile_db_files[@]}" | sort -z)
fi

if [[ "${#tidy_files[@]}" -eq 0 ]]; then
    if [[ -n "$changed_from" ]]; then
        echo "No first-party translation units changed; clang-tidy gate has nothing to check."
        exit 0
    fi
    echo "No first-party translation units found for clang-tidy." >&2
    exit 1
fi

echo "clang-tidy gate: ${#tidy_files[@]} translation unit(s), $jobs job(s), binary $clang_tidy"

set +e
printf '%s\0' "${tidy_files[@]}" |
    xargs -0 -n 4 -P "$jobs" \
        "$clang_tidy" -p "$repo_root/$build_dir" -quiet --config="$gate_config"
tidy_status=$?
set -e

if [[ "$tidy_status" -ne 0 ]]; then
    echo "clang-tidy gate failed (status $tidy_status). Fix the diagnostics above; the checks in scripts/tidy.sh are blocking." >&2
    exit 1
fi

echo "clang-tidy gate passed."
