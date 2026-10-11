#!/usr/bin/env bash
# UCRT64 toolchain package attribution.
#
# Runs inside the MSYS2 UCRT64 shell on one runner. Six sequential cells build
# and time the initial world loading probe while swapping two package cohorts
# (C runtime and GCC) between the installed baseline and older releases:
#   cell0 baseline, cell1 CRT downgrade, cell2 restore,
#   cell3 GCC downgrade, cell4 restore, cell5 baseline repeat.
# Output is numbers and per-cell identity only; interpretation happens outside.
set -euo pipefail

readonly REPO_BASE="https://repo.msys2.org/mingw/ucrt64"
readonly PKG_PREFIX="mingw-w64-ucrt-x86_64"
readonly PACMAN_CACHE="/var/cache/pacman/pkg"
readonly PREFIX="${MSYSTEM_PREFIX:-/ucrt64}"
readonly PERF_FILTER="InitialWorldLoadingPerfTest.MeasuresSurfaceHorizonPrep"
readonly CACHE_DIR="build/pkg-cache"
readonly DIAG_DIR="diag"

CELLS="${CELLS:-0 1 2 3 4 5}"
STEP_TIMEOUT_MINUTES="${STEP_TIMEOUT_MINUTES:-25}"

# Downgrade targets. Only packages that are installed in the baseline are changed.
CRT_NAMES=(crt headers winpthreads libwinpthread)
declare -A CRT_DOWNGRADE=(
    [crt]=14.0.0.r420.g61d40c4c0-1
    [headers]=14.0.0.r420.g61d40c4c0-1
    [winpthreads]=14.0.0.r420.g61d40c4c0-1
    [libwinpthread]=14.0.0.r420.g61d40c4c0-1
)
GCC_NAMES=(gcc gcc-libs cc-libs libgcc libstdc++ libgfortran libgomp libquadmath libatomic binutils)
declare -A GCC_DOWNGRADE=(
    [gcc]=16.2.0-4
    [gcc-libs]=16.2.0-4
    [cc-libs]=16.2.0-4
    [libgcc]=16.2.0-4
    [libstdc++]=16.2.0-4
    [libgfortran]=16.2.0-4
    [libgomp]=16.2.0-4
    [libquadmath]=16.2.0-4
    [libatomic]=16.2.0-4
    [binutils]=2.47-3
)

# Filled from pacman -Q before the first cell runs.
declare -A BASELINE_VERSION=()
declare -A LABEL_OF=([0]=baseline [1]=crt-downgrade [2]=crt-restore
                     [3]=gcc-downgrade [4]=gcc-restore [5]=baseline-repeat)
declare -A ACTION_OF=([0]=none [1]=downgrade-crt [2]=restore
                      [3]=downgrade-gcc [4]=restore [5]=none)
declare -A PREP_OF=()
ROWS_FILE="$DIAG_DIR/rows.md"
ABORT_RUN=0
DIRTY=0

installed_version() {
    local version
    version="$(pacman -Q "$PKG_PREFIX-$1" 2>/dev/null | awk '{print $2}' || true)"
    printf '%s' "$version"
}

record_baseline() {
    local name version
    for name in "${CRT_NAMES[@]}" "${GCC_NAMES[@]}"; do
        version="$(installed_version "$name")"
        if [[ -n "$version" ]]; then
            BASELINE_VERSION[$name]="$version"
        fi
    done
}

# fetch_url URL DEST: download to DEST via a partial file, or fail cleanly.
fetch_url() {
    local url="$1" dest="$2"
    if curl -fsSL --retry 3 -o "$dest.part" "$url"; then
        mv -f "$dest.part" "$dest"
    else
        rm -f "$dest.part"
        return 1
    fi
}

# fetch_package NAME VERSION MODE: print the local path of the package file.
# MODE "restore" prefers the pacman cache; MODE "downgrade" always downloads.
fetch_package() {
    local file="$PKG_PREFIX-$1-$2-any.pkg.tar.zst"
    if [[ "$3" == restore && -f "$PACMAN_CACHE/$file" ]]; then
        printf '%s\n' "$PACMAN_CACHE/$file"
        return 0
    fi
    mkdir -p "$CACHE_DIR"
    fetch_url "$REPO_BASE/$file" "$CACHE_DIR/$file" || return 1
    fetch_url "$REPO_BASE/$file.sig" "$CACHE_DIR/$file.sig" || return 1
    printf '%s\n' "$CACHE_DIR/$file"
}

# verify_pairs NAME=VERSION...: every named package must report that version.
verify_pairs() {
    local pair name version installed status=0
    for pair in "$@"; do
        name="${pair%%=*}"
        version="${pair#*=}"
        installed="$(installed_version "$name")"
        if [[ "$installed" != "$version" ]]; then
            echo "version mismatch: $name expected $version found ${installed:-none}" >&2
            status=1
        fi
    done
    return "$status"
}

# install_pairs MODE NAME=VERSION...: download, pacman -U in one transaction, verify.
install_pairs() {
    local mode="$1"
    shift
    local files=() pair name version path
    if [[ $# -eq 0 ]]; then
        return 0
    fi
    for pair in "$@"; do
        name="${pair%%=*}"
        version="${pair#*=}"
        if ! path="$(fetch_package "$name" "$version" "$mode")"; then
            echo "download failed: $PKG_PREFIX-$name-$version" >&2
            return 1
        fi
        files+=("$path")
    done
    if ! pacman -U --noconfirm "${files[@]}"; then
        echo "pacman -U failed" >&2
        return 1
    fi
    verify_pairs "$@"
}

# downgrade_cohort NAMES_ARRAY TABLE_ARRAY: install the older versions of the
# cohort members that the baseline has.
downgrade_cohort() {
    local -n names_ref="$1"
    local -n table_ref="$2"
    local pairs=() name
    for name in "${names_ref[@]}"; do
        if [[ -n "$(installed_version "$name")" && -n "${table_ref[$name]:-}" ]]; then
            pairs+=("$name=${table_ref[$name]}")
        fi
    done
    DIRTY=1
    install_pairs downgrade "${pairs[@]}"
}

# restore_baseline: reinstall the recorded baseline for every changed member.
restore_baseline() {
    local pairs=() name current
    for name in "${CRT_NAMES[@]}" "${GCC_NAMES[@]}"; do
        if [[ -z "${BASELINE_VERSION[$name]:-}" ]]; then
            continue
        fi
        current="$(installed_version "$name")"
        if [[ "$current" != "${BASELINE_VERSION[$name]}" ]]; then
            pairs+=("$name=${BASELINE_VERSION[$name]}")
        fi
    done
    if install_pairs restore "${pairs[@]}"; then
        DIRTY=0
        return 0
    fi
    return 1
}

verify_baseline() {
    local pairs=() name
    for name in "${!BASELINE_VERSION[@]}"; do
        pairs+=("$name=${BASELINE_VERSION[$name]}")
    done
    verify_pairs "${pairs[@]}"
}

write_identity() {
    local dir="$1" cpu dll
    cpu="$(wmic cpu get name 2>/dev/null | tr -d '\r' | sed -n '2p' | sed 's/[[:space:]]*$//' || true)"
    if [[ -z "$cpu" ]]; then
        cpu="$(sed -n 's/^model name[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo 2>/dev/null | head -n 1 || true)"
    fi
    {
        echo "date_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
        echo "cell=$CELL label=${LABEL_OF[$CELL]}"
        echo "IMAGE_VERSION=${IMAGE_VERSION:-unknown}"
        echo "ImageVersion=${ImageVersion:-unknown} ImageOS=${ImageOS:-unknown}"
        echo "cpu=${cpu:-unknown}"
        echo "which_g++=$(which g++ 2>/dev/null || echo missing)"
        echo "--- g++ -v"
        g++ -v 2>&1 || echo "g++ -v failed"
        echo "--- cmake --version"
        cmake --version 2>&1 | head -n 1 || true
        echo "--- pacman -Q"
        pacman -Q || echo "pacman -Q failed"
        echo "--- runtime dll sha256"
        for dll in libwinpthread-1.dll libstdc++-6.dll libgcc_s_seh-1.dll; do
            if [[ -f "$PREFIX/bin/$dll" ]]; then
                sha256sum "$PREFIX/bin/$dll" || true
            else
                echo "missing $dll"
            fi
        done
    } > "$dir/identity.txt"
}

# finish_cell DIR STATUS PREP WALL: write status.json and the summary row.
finish_cell() {
    local dir="$1" status="$2" prep="$3" wall="$4"
    printf '{"cell": %s, "label": "%s", "status": "%s", "prep_ms": %s, "wall_seconds": %s}\n' \
        "$CELL" "${LABEL_OF[$CELL]}" "$status" "${prep:-null}" "${wall:-null}" > "$dir/status.json"
    printf '| %s | %s | %s | %s | %s |\n' \
        "$CELL" "${LABEL_OF[$CELL]}" "$status" "${prep:-n/a}" "${wall:-n/a}" >> "$ROWS_FILE"
    PREP_OF[$CELL]="${prep:-}"
}

# run_cell CELL: one measured cell. Sets ABORT_RUN=1 when a restore cannot
# reach the recorded baseline; every other failure is recorded and returns.
run_cell() {
    CELL="$1"
    local action="${ACTION_OF[$CELL]}"
    local dir="$DIAG_DIR/cell-$CELL" bdir="build/cell-$CELL"
    local status="OK" prep="" wall="" rc=0 start end targets=(initial_world_loading_perf_test)
    local exe

    rm -rf "$dir" "$bdir"
    mkdir -p "$dir"

    case "$action" in
        downgrade-crt)
            if ! downgrade_cohort CRT_NAMES CRT_DOWNGRADE; then
                restore_baseline || true
                finish_cell "$dir" INSTALL_FAILED "" ""
                return 0
            fi
            ;;
        downgrade-gcc)
            if ! downgrade_cohort GCC_NAMES GCC_DOWNGRADE; then
                restore_baseline || true
                finish_cell "$dir" INSTALL_FAILED "" ""
                return 0
            fi
            ;;
        restore)
            if ! restore_baseline || ! verify_baseline; then
                finish_cell "$dir" RESTORE_FAILED "" ""
                ABORT_RUN=1
                return 0
            fi
            ;;
        none)
            if ! verify_baseline; then
                finish_cell "$dir" RESTORE_FAILED "" ""
                ABORT_RUN=1
                return 0
            fi
            ;;
    esac

    write_identity "$dir"

    if ! cmake --preset release -B "$bdir" > "$dir/configure.log" 2>&1; then
        finish_cell "$dir" BUILD_FAILED "" ""
        return 0
    fi

    cmake --build "$bdir" --target help > "$dir/targets.txt" 2>/dev/null || true
    if grep -qw worldgen_probe "$dir/targets.txt"; then
        targets+=(worldgen_probe)
    fi

    rc=0
    timeout "${STEP_TIMEOUT_MINUTES}m" cmake --build "$bdir" --target "${targets[@]}" --parallel 4 \
        > "$dir/build.log" 2>&1 || rc=$?
    if (( rc == 124 )); then
        finish_cell "$dir" TIMED_OUT "" ""
        return 0
    elif (( rc != 0 )); then
        finish_cell "$dir" BUILD_FAILED "" ""
        return 0
    fi

    exe="$bdir/bin/initial_world_loading_perf_test"
    if [[ -f "$exe.exe" ]]; then
        exe="$exe.exe"
    fi
    if [[ ! -f "$exe" ]]; then
        finish_cell "$dir" BUILD_FAILED "" ""
        return 0
    fi
    {
        echo "--- produced test binary"
        sha256sum "$exe" || true
    } >> "$dir/identity.txt"

    start="$(date +%s.%N)"
    rc=0
    timeout "${STEP_TIMEOUT_MINUTES}m" "$exe" --gtest_filter="$PERF_FILTER" \
        > "$dir/perf.log" 2>&1 || rc=$?
    end="$(date +%s.%N)"
    wall="$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.3f", b - a }')"
    if (( rc == 124 )); then
        status=TIMED_OUT
    elif (( rc != 0 )); then
        status=TEST_FAILED
    fi
    prep="$(sed -n 's/.*surface ready (took \([0-9][0-9]*\)ms).*/\1/p' "$dir/perf.log" | tail -n 1 || true)"
    if [[ "$status" == OK && -z "$prep" ]]; then
        status=TEST_FAILED
    fi

    if [[ "$status" == OK && " ${targets[*]} " == *" worldgen_probe "* ]]; then
        rc=0
        timeout "${STEP_TIMEOUT_MINUTES}m" "$bdir/bin/worldgen_probe" --json "$dir/probe.json" \
            > "$dir/probe.txt" 2>&1 || rc=$?
        if (( rc == 124 )); then
            status=TIMED_OUT
        elif (( rc != 0 )); then
            status=TEST_FAILED
        fi
    fi

    finish_cell "$dir" "$status" "$prep" "$wall"
    return 0
}

write_summary() {
    local out="$DIAG_DIR/summary.md" spread="n/a" first last delta
    first="${PREP_OF[0]:-}"
    last="${PREP_OF[5]:-}"
    if [[ -n "$first" && -n "$last" ]]; then
        if (( last > first )); then delta=$(( last - first )); else delta=$(( first - last )); fi
        spread="$delta"
    fi
    {
        echo "## UCRT64 package attribution"
        echo
        echo "| Cell | Label | Status | prep_ms | wall_seconds |"
        echo "| --- | --- | --- | --- | --- |"
        if [[ -f "$ROWS_FILE" ]]; then cat "$ROWS_FILE"; fi
        echo
        echo "cell0 versus cell5 prep_ms spread: $spread"
    } > "$out"
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" ]]; then
        cat "$out" >> "$GITHUB_STEP_SUMMARY"
    fi
}

main() {
    local cell
    mkdir -p "$DIAG_DIR"
    record_baseline
    : > "$ROWS_FILE"
    for cell in $CELLS; do
        if [[ ! "$cell" =~ ^[0-5]$ ]]; then
            echo "invalid cell: $cell" >&2
            exit 2
        fi
    done
    for cell in $CELLS; do
        run_cell "$cell" || true
        if (( ABORT_RUN )); then
            echo "restore did not reach the recorded baseline; stopping after cell $cell" >&2
            break
        fi
    done
    if (( DIRTY )); then
        restore_baseline || echo "final restore to baseline failed" >&2
    fi
    write_summary
}

main "$@"
