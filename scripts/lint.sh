#!/usr/bin/env bash
set -euo pipefail

build_dir="build"
format_only=0
tidy_only=0
changed_from=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build-dir)
            build_dir="$2"
            shift 2
            ;;
        --format-only)
            format_only=1
            shift
            ;;
        --tidy-only)
            tidy_only=1
            shift
            ;;
        --changed-from)
            changed_from="$2"
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

for source_root in src include test; do
    if [[ ! -d "$repo_root/$source_root" ]]; then
        echo "Required source directory is missing: $source_root" >&2
        exit 1
    fi
done

if [[ -n "$changed_from" ]]; then
    if ! git -C "$repo_root" rev-parse --verify --quiet "$changed_from^{commit}" >/dev/null; then
        echo "Formatting comparison commit does not exist: $changed_from" >&2
        exit 1
    fi

    cpp_files=()
    while IFS= read -r -d '' relative_path; do
        case "$relative_path" in
            src/*|include/*|test/*)
                case "$relative_path" in
                    *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx)
                        cpp_files+=("$repo_root/$relative_path")
                        ;;
                esac
                ;;
        esac
    done < <(git -C "$repo_root" diff --name-only --diff-filter=ACMR -z "$changed_from")
else
    mapfile -t cpp_files < <(
        find "$repo_root/src" "$repo_root/include" "$repo_root/test" \
            -type f \
            \( -name '*.c' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx' -o \
               -name '*.h' -o -name '*.hh' -o -name '*.hpp' -o -name '*.hxx' \) \
            ! -path '*/vendor/*' \
            ! -path '*/external/*' \
            ! -path '*/build/*' \
            ! -path '*/out/*'
    )
fi

if [[ "${#cpp_files[@]}" -eq 0 ]]; then
    if [[ -n "$changed_from" ]]; then
        echo "No first-party C/C++ files changed."
        exit 0
    fi
    echo "No first-party C/C++ files found." >&2
    exit 1
fi

if [[ "$tidy_only" -eq 0 ]]; then
    if ! command -v clang-format >/dev/null 2>&1; then
        echo "clang-format was not found on PATH." >&2
        exit 1
    fi

    clang-format --dry-run --Werror "${cpp_files[@]}"
fi

if [[ "$format_only" -eq 0 ]]; then
    if ! command -v clang-tidy >/dev/null 2>&1; then
        echo "clang-tidy was not found on PATH." >&2
        exit 1
    fi

    compile_db="$repo_root/$build_dir/compile_commands.json"
    if [[ ! -f "$compile_db" ]]; then
        echo "clang-tidy requires $compile_db. Configure CMake with compile commands enabled first." >&2
        exit 1
    fi

    tidy_files=()
    for file in "${cpp_files[@]}"; do
        case "$file" in
            *.c|*.cc|*.cpp|*.cxx)
                tidy_files+=("$file")
                ;;
        esac
    done

    if [[ "${#tidy_files[@]}" -eq 0 ]]; then
        echo "No first-party C/C++ translation units found for clang-tidy." >&2
        exit 1
    fi

    clang-tidy -p "$repo_root/$build_dir" "${tidy_files[@]}"
fi
