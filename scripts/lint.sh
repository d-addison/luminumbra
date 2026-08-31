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

for source_root in src include test tools; do
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
    untracked_cpp_files=()
    while IFS= read -r -d '' relative_path; do
        case "$relative_path" in
            src/*|include/*|test/*|tools/*)
                case "$relative_path" in
                    *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx)
                        cpp_files+=("$repo_root/$relative_path")
                        ;;
                esac
                ;;
        esac
    done < <(git -C "$repo_root" diff --name-only --diff-filter=ACMR -z "$changed_from")

    while IFS= read -r -d '' relative_path; do
        case "$relative_path" in
            src/*|include/*|test/*|tools/*)
                case "$relative_path" in
                    *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx)
                        untracked_cpp_files+=("$repo_root/$relative_path")
                        cpp_files+=("$repo_root/$relative_path")
                        ;;
                esac
                ;;
        esac
    done < <(git -C "$repo_root" ls-files --others --exclude-standard -z -- src include test tools)
else
    mapfile -t cpp_files < <(
        find "$repo_root/src" "$repo_root/include" "$repo_root/test" "$repo_root/tools" \
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

    if [[ -n "$changed_from" ]]; then
        clang_format_diff=""
        for candidate in clang-format-diff clang-format-diff-18 clang-format-diff-17; do
            if command -v "$candidate" >/dev/null 2>&1; then
                clang_format_diff="$candidate"
                break
            fi
        done
        if [[ -z "$clang_format_diff" ]]; then
            echo "clang-format-diff was not found on PATH." >&2
            exit 1
        fi
        set +e
        format_diff=$(
            git -C "$repo_root" diff --no-ext-diff -U0 "$changed_from" -- \
                'src/**/*.c' 'src/**/*.cc' 'src/**/*.cpp' 'src/**/*.cxx' \
                'src/**/*.h' 'src/**/*.hh' 'src/**/*.hpp' 'src/**/*.hxx' \
                'include/**/*.c' 'include/**/*.cc' 'include/**/*.cpp' 'include/**/*.cxx' \
                'include/**/*.h' 'include/**/*.hh' 'include/**/*.hpp' 'include/**/*.hxx' \
                'test/**/*.c' 'test/**/*.cc' 'test/**/*.cpp' 'test/**/*.cxx' \
                'test/**/*.h' 'test/**/*.hh' 'test/**/*.hpp' 'test/**/*.hxx' \
                'tools/**/*.c' 'tools/**/*.cc' 'tools/**/*.cpp' 'tools/**/*.cxx' \
                'tools/**/*.h' 'tools/**/*.hh' 'tools/**/*.hpp' 'tools/**/*.hxx' |
                "$clang_format_diff" -p1 -style=file
        )
        format_status=$?
        set -e
        if [[ "$format_status" -gt 1 ]]; then
            echo "clang-format-diff failed with status $format_status." >&2
            exit "$format_status"
        fi
        if [[ -n "$format_diff" ]]; then
            echo "$format_diff"
            exit 1
        fi
        if [[ "${#untracked_cpp_files[@]}" -gt 0 ]]; then
            clang-format --dry-run --Werror -style=file "${untracked_cpp_files[@]}"
        fi
    else
        clang-format --dry-run --Werror "${cpp_files[@]}"
    fi
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
