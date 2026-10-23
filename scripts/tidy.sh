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
#   scripts/tidy.sh --files0-from FILE   # strict NUL-delimited dependency-expanded TU list
#   scripts/tidy.sh --changed-from REF    # analyze TUs changed since REF
#   scripts/tidy.sh --build-dir DIR       # compile db location (default build/tidy)
#   scripts/tidy.sh --jobs N              # parallel clang-tidy processes
#   scripts/tidy.sh --shard INDEX/COUNT   # stable path-hash partition (default 1/1)
#   scripts/tidy.sh --cache-dir DIR       # skip units proven identical to a passing run
#   scripts/tidy.sh --cache-mode MODE     # readwrite (default) or writeonly (never skip)

build_dir="build/tidy"
changed_from=""
files0_from=""
jobs="$(nproc)"
cache_dir=""
cache_mode="readwrite"
shard="1/1"

while [[ $# -gt 0 ]]; do
    if [[ $# -lt 2 || -z "$2" ]]; then
        echo "$1 requires a nonempty value" >&2
        exit 2
    fi
    case "$1" in
        --files0-from)
            files0_from="$2"
            shift 2
            ;;
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
        --shard)
            if [[ $# -lt 2 ]]; then
                echo "--shard requires INDEX/COUNT with 1 <= INDEX <= COUNT" >&2
                exit 2
            fi
            shard="$2"
            shift 2
            ;;
        --cache-dir)
            cache_dir="$2"
            shift 2
            ;;
        --cache-mode)
            cache_mode="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1" >&2
            exit 2
            ;;
    esac
done

if [[ -n "$files0_from" && -n "$changed_from" ]]; then
    echo "--files0-from and --changed-from are mutually exclusive" >&2
    exit 2
fi
if [[ ! "$jobs" =~ ^[0-9]+$ || "$jobs" =~ ^0+$ ]]; then
    echo "--jobs requires a positive integer" >&2
    exit 2
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/.." && pwd)"

# Validate before looking for analysis tools, even when this shard will be empty.
python3 "$script_dir/tidy_cache.py" shard --repo-root "$repo_root" --shard "$shard"

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
gate_checks+=",performance-unnecessary-value-param"
gate_checks+=",cppcoreguidelines-avoid-non-const-global-variables"
gate_checks+=",readability-function-cognitive-complexity"

# Keep every other blocking check enabled for translation units with known
# non-const global state while enforcing the check everywhere else.
tracked_global_tus=(
    # Owns the four process-wide GL-debug diagnostic counters.
    "src/luminumbra_client/core/RuntimeScenarioConfig.cpp"
    # Owns the GNS initialization flag and live-transport callback registry.
    "src/luminumbra_common/net/GnsTransport.cpp"
)
tracked_global_gate_checks="$gate_checks,-cppcoreguidelines-avoid-non-const-global-variables"

# Threshold 250 is the smallest round value that grandfathers every existing
# function (current worst first-party function scores 243).
gate_config='{
  "Checks": "'"$gate_checks"'",
  "WarningsAsErrors": "*",
  "HeaderFilterRegex": "^(src|include|test)/",
  "FormatStyle": "file",
  "CheckOptions": [
    {"key": "readability-function-cognitive-complexity.Threshold", "value": "250"},
    {"key": "modernize-use-nullptr.NullMacros", "value": "NULL"}
  ]
}'

tracked_global_gate_config='{
  "Checks": "'"$tracked_global_gate_checks"'",
  "WarningsAsErrors": "*",
  "HeaderFilterRegex": "^(src|include|test)/",
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
    echo "First initialize a fresh build root: python3 -I -B tools/ci/rmlui_source.py init --root $build_dir" >&2
    echo "Example: cmake \"-DPython3_EXECUTABLE=\$(python3 -I -B -c 'import sys; print(sys.executable)')\" -S . -B $build_dir -G Ninja -DCMAKE_BUILD_TYPE=Release \\" >&2
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
wait "$!"

# First-party translation units live under src/, test/ and tools/. The RmlUi GL3
# backend is vendored upstream code carried in-tree and is excluded from the
# gate.
is_first_party_tu() {
    case "$1" in
        src/luminumbra_client/ui/gl3/*)
            return 1
            ;;
        src/*.c|src/*.cc|src/*.cpp|src/*.cxx| \
            test/*.c|test/*.cc|test/*.cpp|test/*.cxx| \
            tools/*.c|tools/*.cc|tools/*.cpp|tools/*.cxx)
            return 0
            ;;
    esac
    return 1
}

tidy_files=()
if [[ -n "$files0_from" ]]; then
    # This checks the supplied selection, never discovers or silently drops TUs.
    # Python resolves relative database files against their command directories.
    mapfile -d '' -t tidy_files < <(python3 - "$repo_root" "$compile_db" "$files0_from" <<'PYFILES'
import json
import os
from pathlib import Path
import sys

root = Path(sys.argv[1]).resolve()
try:
    data = Path(sys.argv[3]).read_bytes()
    if data and (not data.endswith(b"\0") or b"\0\0" in data):
        raise ValueError("expected nonempty NUL-terminated file names")
    database = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
    known = {(Path(e.get("directory", root)) / e["file"]).resolve() for e in database}
    selected = set()
    for raw in data.split(b"\0")[:-1]:
        path = (root / os.fsdecode(raw)).resolve()
        if not path.is_relative_to(root) or not path.is_file() or path not in known:
            raise ValueError("selected TU missing, outside checkout or absent from database: " + os.fsdecode(raw))
        selected.add(path)
    for path in sorted(selected):
        sys.stdout.buffer.write(os.fsencode(path) + b"\0")
except (OSError, ValueError, TypeError, KeyError) as error:
    sys.exit("Invalid --files0-from selection: " + str(error))
PYFILES
    )
    wait "$!"
    for tidy_file in "${tidy_files[@]}"; do
        if ! is_first_party_tu "${tidy_file#"$repo_root/"}"; then
            echo "Unsupported first-party TU in --files0-from: $tidy_file" >&2
            exit 1
        fi
    done
elif [[ -n "$changed_from" ]]; then
    if ! git -C "$repo_root" rev-parse --verify --quiet "$changed_from^{commit}" >/dev/null; then
        echo "Tidy comparison commit does not exist: $changed_from" >&2
        exit 1
    fi

    while IFS= read -r -d '' relative_path; do
        if is_first_party_tu "$relative_path" &&
            [[ -n "${compile_db_files[$repo_root/$relative_path]:-}" ]]; then
            tidy_files+=("$repo_root/$relative_path")
        fi
    done < <(git -C "$repo_root" diff --name-only --diff-filter=ACMR -z "$changed_from" -- src test tools)

    while IFS= read -r -d '' relative_path; do
        if is_first_party_tu "$relative_path" &&
            [[ -n "${compile_db_files[$repo_root/$relative_path]:-}" ]]; then
            tidy_files+=("$repo_root/$relative_path")
        fi
    done < <(git -C "$repo_root" ls-files --others --exclude-standard -z -- src test tools)
else
    while IFS= read -r -d '' absolute_path; do
        if is_first_party_tu "${absolute_path#"$repo_root/"}"; then
            tidy_files+=("$absolute_path")
        fi
    done < <(printf '%s\0' "${!compile_db_files[@]}" | sort -z)
fi

# Preserve the full-scope discovery guard; an empty partition of a valid TU list
# is handled successfully below.
if [[ "${#tidy_files[@]}" -eq 0 && -z "$changed_from" && -z "$files0_from" ]]; then
    echo "No first-party translation units found for clang-tidy." >&2
    exit 1
fi

# Partition only after compile-database discovery and changed-file filtering,
# before splitting the selected units by their tracked-global configuration.
mapfile -d '' -t tidy_files < <(
    python3 "$script_dir/tidy_cache.py" shard --repo-root "$repo_root" --shard "$shard" -- "${tidy_files[@]}"
)
# Propagate a helper failure instead of treating it as an empty, passing shard.
wait "$!"

echo "clang-tidy gate: shard $shard, ${#tidy_files[@]} translation unit(s), $jobs job(s), binary $clang_tidy"
if [[ "${#tidy_files[@]}" -eq 0 ]]; then
    echo "clang-tidy shard $shard has nothing to analyse."
    exit 0
fi

regular_tidy_files=()
tracked_global_tidy_files=()
for tidy_file in "${tidy_files[@]}"; do
    relative_path="${tidy_file#"$repo_root/"}"
    is_tracked_global_tu=false
    for tracked_global_tu in "${tracked_global_tus[@]}"; do
        if [[ "$relative_path" == "$tracked_global_tu" ]]; then
            is_tracked_global_tu=true
            break
        fi
    done
    if [[ "$is_tracked_global_tu" == true ]]; then
        tracked_global_tidy_files+=("$tidy_file")
    else
        regular_tidy_files+=("$tidy_file")
    fi
done

run_tidy_batch() {
    local config="$1"
    shift
    if [[ "$#" -eq 0 ]]; then
        return 0
    fi
    if [[ -n "$cache_dir" ]]; then
        # Cached mode: tools/ci/test_tidy_cache.py documents why a hit can
        # only ever stand in for a previous passing run of identical inputs.
        python3 "$script_dir/tidy_cache.py" run \
            --build-dir "$repo_root/$build_dir" --cache-dir "$cache_dir" \
            --mode "$cache_mode" --jobs "$jobs" --clang-tidy "$clang_tidy" \
            --config "$config" --tool-file "$script_dir/tidy.sh" -- "$@"
        return $?
    fi
    printf '%s\0' "$@" |
        xargs -0 -n 4 -P "$jobs" \
            "$clang_tidy" -p "$repo_root/$build_dir" -quiet --config="$config"
}

case "$cache_mode" in
    readwrite | writeonly) ;;
    *)
        echo "Unknown --cache-mode: $cache_mode" >&2
        exit 2
        ;;
esac
cache_started="$(date +%s)"

set +e
run_tidy_batch "$gate_config" "${regular_tidy_files[@]}"
tidy_status=$?
run_tidy_batch "$tracked_global_gate_config" "${tracked_global_tidy_files[@]}"
tracked_global_tidy_status=$?
set -e

if [[ "$tidy_status" -ne 0 || "$tracked_global_tidy_status" -ne 0 ]]; then
    echo "clang-tidy gate failed (status $tidy_status). Fix the diagnostics above; the checks in scripts/tidy.sh are blocking." >&2
    exit 1
fi

if [[ -n "$cache_dir" ]]; then
    # Keep only the records this run used, so the saved cache tracks the
    # current tree instead of growing with every commit.
    python3 "$script_dir/tidy_cache.py" prune --cache-dir "$cache_dir" --before "$cache_started"
fi

echo "clang-tidy gate passed."
