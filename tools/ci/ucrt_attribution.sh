#!/usr/bin/env bash
# UCRT64 toolchain package attribution.
#
# Runs inside the MSYS2 UCRT64 shell on one runner. Six sequential cells build
# and time the initial world loading probe while swapping two package cohorts
# (C runtime and GCC) between the installed baseline and older releases:
#   cell0 baseline, cell1 CRT downgrade, cell2 restore,
#   cell3 GCC downgrade, cell4 restore, cell5 baseline repeat.
#
# Every baseline archive is staged before the first cell. Each intervention cell
# first restores and verifies the baseline, so interventions never combine.
# Output is numbers and per-cell identity only; interpretation happens outside.
set -euo pipefail

readonly REPO_BASE="https://repo.msys2.org/mingw/ucrt64"
readonly PKG_PREFIX="mingw-w64-ucrt-x86_64"
readonly PREFIX="${MSYSTEM_PREFIX:-/ucrt64}"
readonly PERF_FILTER="InitialWorldLoadingPerfTest.MeasuresSurfaceHorizonPrep"
readonly CACHE_DIR="build/pkg-cache"
readonly BASELINE_DIR="$CACHE_DIR/baseline"
readonly DIAG_DIR="diag"
readonly ROWS_FILE="$DIAG_DIR/rows.md"
readonly SUMMARY_FILE="$DIAG_DIR/summary.md"
readonly RUNTIME_DLLS=(libwinpthread-1.dll libstdc++-6.dll libgcc_s_seh-1.dll)
readonly BUILD_PROCESS_NAMES=(ninja cmake cc1plus cc1 collect2 ld as gcc g++)

CELLS="${CELLS:-0 1 2 3 4 5}"
STEP_TIMEOUT_MINUTES="${STEP_TIMEOUT_MINUTES:-25}"
# Total budget for one cell (or the baseline staging pass). Every download,
# package transaction, configure, build and test runs under the remaining budget.
CELL_TIMEOUT_MINUTES="${CELL_TIMEOUT_MINUTES:-45}"

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
EXPERIMENT_STATUS=RUNNING
ABORT_RUN=0
DIRTY=0
CELL=""
CURRENT_CELL=""
CELL_START=0
CELL_PROBE=NOT_RUN
CELL_PERF=NOT_RUN
CELL_PREP=""
CELL_WALL=""

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

# begin_deadline: start the budget for the next bounded operation group.
begin_deadline() {
    CELL_START="$(date +%s)"
}

# cell_budget: print the time limit for one operation, capped by the cell
# budget that remains. Fails when the budget is exhausted.
cell_budget() {
    local left step
    left=$(( CELL_START + CELL_TIMEOUT_MINUTES * 60 - $(date +%s) ))
    if (( left <= 0 )); then
        return 1
    fi
    step=$(( STEP_TIMEOUT_MINUTES * 60 ))
    if (( left < step )); then
        printf '%ss' "$left"
    else
        printf '%sm' "$STEP_TIMEOUT_MINUTES"
    fi
}

# bounded COMMAND...: run under the cell deadline, with a forced kill 30 s after
# the limit. An exhausted budget reports 124 without starting the command.
bounded() {
    local limit
    limit="$(cell_budget)" || return 124
    timeout --kill-after=30s "$limit" "$@"
}

is_timeout_rc() {
    (( $1 == 124 || $1 == 137 ))
}

classify_rc() {
    local rc="$1" failure="$2"
    if (( rc == 0 )); then
        echo OK
    elif is_timeout_rc "$rc"; then
        echo TIMED_OUT
    else
        echo "$failure"
    fi
}

# retire_path PATH: remove a tree and verify it is gone, retrying briefly.
retire_path() {
    local path="$1" attempt
    for attempt in 1 2 3; do
        rm -rf -- "$path" 2>/dev/null || true
        if [[ ! -e "$path" ]]; then
            return 0
        fi
        sleep 2
    done
    echo "could not remove $path" >&2
    return 1
}

# ensure_no_build_descendants: no compiler, linker or build-tool process may
# survive before a package swap. Fails closed when the process tools are absent.
ensure_no_build_descendants() {
    local name attempt remaining
    if ! command -v pgrep >/dev/null 2>&1 || ! command -v pkill >/dev/null 2>&1; then
        echo "pgrep/pkill unavailable; cannot verify build descendants" >&2
        return 1
    fi
    for attempt in 1 2 3; do
        remaining=0
        for name in "${BUILD_PROCESS_NAMES[@]}"; do
            if pgrep -x "$name" >/dev/null 2>&1; then
                pkill -KILL -x "$name" >/dev/null 2>&1 || true
                remaining=1
            fi
        done
        if (( remaining == 0 )); then
            return 0
        fi
        sleep 2
    done
    for name in "${BUILD_PROCESS_NAMES[@]}"; do
        if pgrep -x "$name" >/dev/null 2>&1; then
            echo "build process still running: $name" >&2
            return 1
        fi
    done
    return 0
}

# fetch_url URL DEST: download to DEST via a partial file, or fail cleanly.
fetch_url() {
    local url="$1" dest="$2"
    if bounded curl -fsSL --retry 3 -o "$dest.part" "$url"; then
        mv -f "$dest.part" "$dest" || return 1
    else
        rm -f "$dest.part"
        return 1
    fi
}

# package_path MODE NAME VERSION: print the local archive path. MODE "restore"
# reads only the staged baseline set; MODE "downgrade" downloads the target.
package_path() {
    local mode="$1" file="$PKG_PREFIX-$2-$3-any.pkg.tar.zst"
    if [[ "$mode" == restore ]]; then
        if [[ ! -s "$BASELINE_DIR/$file" || ! -s "$BASELINE_DIR/$file.sig" ]]; then
            return 1
        fi
        printf '%s\n' "$BASELINE_DIR/$file"
        return 0
    fi
    mkdir -p "$CACHE_DIR" || return 1
    fetch_url "$REPO_BASE/$file" "$CACHE_DIR/$file" || return 1
    fetch_url "$REPO_BASE/$file.sig" "$CACHE_DIR/$file.sig" || return 1
    printf '%s\n' "$CACHE_DIR/$file"
}

# stage_baseline: download every recorded baseline archive and signature and
# record their hashes. Fails unless the complete restore set is present.
stage_baseline() {
    local name version file suffix missing=0
    local hashes="$DIAG_DIR/baseline-sha256.txt"
    begin_deadline
    mkdir -p "$BASELINE_DIR" || return 1
    : > "$hashes" || return 1
    for name in "${CRT_NAMES[@]}" "${GCC_NAMES[@]}"; do
        version="${BASELINE_VERSION[$name]:-}"
        if [[ -z "$version" ]]; then
            continue
        fi
        file="$PKG_PREFIX-$name-$version-any.pkg.tar.zst"
        for suffix in "" .sig; do
            if [[ ! -s "$BASELINE_DIR/$file$suffix" ]]; then
                if ! fetch_url "$REPO_BASE/$file$suffix" "$BASELINE_DIR/$file$suffix"; then
                    echo "staging failed: $file$suffix" >&2
                    missing=1
                    continue
                fi
            fi
            if [[ ! -s "$BASELINE_DIR/$file$suffix" ]]; then
                echo "staged file missing: $file$suffix" >&2
                missing=1
                continue
            fi
            sha256sum "$BASELINE_DIR/$file$suffix" >> "$hashes" || return 1
        done
    done
    if (( missing )); then
        return 1
    fi
    return 0
}

# install_pairs MODE NAME=VERSION...: one pacman -U transaction, then verify.
install_pairs() {
    local mode="$1" pair name version path
    shift
    local files=()
    if [[ $# -eq 0 ]]; then
        return 0
    fi
    if ! ensure_no_build_descendants; then
        echo "package swap refused: build processes still present" >&2
        return 1
    fi
    for pair in "$@"; do
        name="${pair%%=*}"
        version="${pair#*=}"
        if ! path="$(package_path "$mode" "$name" "$version")"; then
            echo "package unavailable ($mode): $PKG_PREFIX-$name-$version" >&2
            return 1
        fi
        files+=("$path")
    done
    if ! bounded pacman -U --noconfirm "${files[@]}"; then
        echo "pacman -U failed or timed out" >&2
        return 1
    fi
    verify_pairs "$@"
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

# downgrade_cohort NAMES_ARRAY TABLE_ARRAY: install the older versions of the
# cohort members that the baseline has. An empty cohort is a failed intervention.
downgrade_cohort() {
    local -n names_ref="$1"
    local -n table_ref="$2"
    local pairs=() name
    for name in "${names_ref[@]}"; do
        if [[ -n "$(installed_version "$name")" && -n "${table_ref[$name]:-}" ]]; then
            pairs+=("$name=${table_ref[$name]}")
        fi
    done
    if (( ${#pairs[@]} == 0 )); then
        echo "no installed cohort members to downgrade" >&2
        return 1
    fi
    DIRTY=1
    install_pairs downgrade "${pairs[@]}"
}

# restore_baseline: reinstall the recorded baseline for every changed member,
# using only the staged archives.
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
    for name in "${CRT_NAMES[@]}" "${GCC_NAMES[@]}"; do
        if [[ -n "${BASELINE_VERSION[$name]:-}" ]]; then
            pairs+=("$name=${BASELINE_VERSION[$name]}")
        fi
    done
    verify_pairs "${pairs[@]}"
}

restore_to_baseline() {
    restore_baseline && verify_baseline
}

write_identity() {
    local dir="$1" cpu dll file="$1/identity.txt"
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
        echo "--- source runtime dll sha256 (package prefix)"
        for dll in "${RUNTIME_DLLS[@]}"; do
            if [[ -f "$PREFIX/bin/$dll" ]]; then
                sha256sum "$PREFIX/bin/$dll" || true
            else
                echo "missing $dll"
            fi
        done
    } > "$file" || return 1
}

# stage_runtime DIR BINDIR EXE...: copy the runtime DLLs beside the executables,
# verify each copy against its source, and hash the copies and executables
# before any measurement starts.
stage_runtime() {
    local dir="$1" bindir="$2" dll exe
    shift 2
    for dll in "${RUNTIME_DLLS[@]}"; do
        if ! cp -f -- "$PREFIX/bin/$dll" "$bindir/$dll"; then
            echo "runtime stage failed: $dll" >&2
            return 1
        fi
        if ! cmp -s -- "$PREFIX/bin/$dll" "$bindir/$dll"; then
            echo "staged runtime differs from source: $dll" >&2
            return 1
        fi
    done
    {
        echo "--- staged runtime dll sha256 (beside executables)"
        for dll in "${RUNTIME_DLLS[@]}"; do
            sha256sum "$bindir/$dll" || return 1
        done
        echo "--- measured executable sha256"
        for exe in "$@"; do
            sha256sum "$exe" || return 1
        done
    } >> "$dir/identity.txt" || return 1
}

# copy_test_artifacts BDIR DIR: keep whatever performance reports the test wrote,
# including after a nonzero exit. Missing reports are not an error.
copy_test_artifacts() {
    local src="$1/test-artifacts/performance"
    if [[ ! -d "$src" ]]; then
        return 0
    fi
    if ! cp -R -- "$src"/. "$2"/; then
        echo "artifact copy failed: $src" >&2
    fi
}

# finish_cell DIR STATUS: write status.json and the summary row for CELL.
finish_cell() {
    local dir="$1" status="$2" prep="${CELL_PREP:-}" wall="${CELL_WALL:-}"
    printf '{"cell": %s, "label": "%s", "status": "%s", "probe_status": "%s", "perf_status": "%s", "prep_ms": %s, "wall_seconds": %s}\n' \
        "$CELL" "${LABEL_OF[$CELL]}" "$status" "$CELL_PROBE" "$CELL_PERF" \
        "${prep:-null}" "${wall:-null}" > "$dir/status.json" || return 1
    printf '| %s | %s | %s | %s | %s | %s | %s |\n' \
        "$CELL" "${LABEL_OF[$CELL]}" "$status" "$CELL_PROBE" "$CELL_PERF" \
        "${prep:-n/a}" "${wall:-n/a}" >> "$ROWS_FILE" || return 1
    PREP_OF[$CELL]="$prep"
    CURRENT_CELL=""
}

# run_probe DIR BDIR EXE: the probe runs first and records its own outcome.
run_probe() {
    local dir="$1" bdir="$2" exe="$3" rc=0
    bounded "$exe" --json "$dir/probe.json" > "$dir/probe.txt" 2>&1 || rc=$?
    CELL_PROBE="$(classify_rc "$rc" TEST_FAILED)"
    if [[ "$CELL_PROBE" == OK && ! -s "$dir/probe.json" ]]; then
        CELL_PROBE=TEST_FAILED
    fi
    copy_test_artifacts "$bdir" "$dir"
}

# run_perf DIR BDIR EXE: the perf test records its own outcome, independent of the probe.
run_perf() {
    local dir="$1" bdir="$2" exe="$3" rc=0 start end
    start="$(date +%s.%N)"
    bounded "$exe" --gtest_filter="$PERF_FILTER" > "$dir/perf.log" 2>&1 || rc=$?
    end="$(date +%s.%N)"
    CELL_WALL="$(awk -v a="$start" -v b="$end" 'BEGIN { printf "%.3f", b - a }')"
    CELL_PERF="$(classify_rc "$rc" TEST_FAILED)"
    CELL_PREP="$(sed -n 's/.*surface ready (took \([0-9][0-9]*\)ms).*/\1/p' "$dir/perf.log" | tail -n 1 || true)"
    if [[ "$CELL_PERF" == OK && -z "$CELL_PREP" ]]; then
        CELL_PERF=TEST_FAILED
    fi
    copy_test_artifacts "$bdir" "$dir"
}

resolve_exe() {
    if [[ -f "$1.exe" ]]; then
        printf '%s' "$1.exe"
    else
        printf '%s' "$1"
    fi
}

build_failure_status() {
    if is_timeout_rc "$1"; then
        echo TIMED_OUT
    else
        echo BUILD_FAILED
    fi
}

# apply_intervention ACTION: apply one package cohort downgrade, if the action is one.
apply_intervention() {
    case "$1" in
        downgrade-crt) downgrade_cohort CRT_NAMES CRT_DOWNGRADE ;;
        downgrade-gcc) downgrade_cohort GCC_NAMES GCC_DOWNGRADE ;;
        *) return 0 ;;
    esac
}

# run_cell CELL: one measured cell. Expected failures are recorded and return 0.
# Sets ABORT_RUN=1 when the baseline cannot be restored. Any other error is
# unexpected and stops the run through errexit, after the EXIT trap.
run_cell() {
    CELL="$1"
    CURRENT_CELL="$CELL"
    CELL_PROBE=NOT_RUN
    CELL_PERF=NOT_RUN
    CELL_PREP=""
    CELL_WALL=""
    local action="${ACTION_OF[$CELL]}"
    local dir="$DIAG_DIR/cell-$CELL" bdir="build/cell-$CELL"
    local has_probe=0 perf_exe="" probe_exe="" candidate="" bindir="" rc=0 status
    local -a exes=()

    begin_deadline
    if ! retire_path "$dir" || ! retire_path "$bdir"; then
        mkdir -p "$dir" || return 1
        finish_cell "$dir" CLEANUP_FAILED || return 1
        return 0
    fi
    mkdir -p "$dir" || return 1

    # Every intervention is preceded by a verified restore, whatever cells were
    # selected. Cells 0 and 5 only verify the baseline.
    if [[ "$action" == none ]]; then
        if ! verify_baseline; then
            finish_cell "$dir" RESTORE_FAILED || return 1
            ABORT_RUN=1
            return 0
        fi
    elif ! restore_to_baseline; then
        finish_cell "$dir" RESTORE_FAILED || return 1
        ABORT_RUN=1
        return 0
    fi

    if ! apply_intervention "$action"; then
        if ! restore_to_baseline; then
            ABORT_RUN=1
        fi
        finish_cell "$dir" INSTALL_FAILED || return 1
        return 0
    fi

    write_identity "$dir" || return 1

    rc=0
    bounded cmake --preset release -B "$bdir" > "$dir/configure.log" 2>&1 || rc=$?
    if (( rc != 0 )); then
        finish_cell "$dir" "$(build_failure_status "$rc")" || return 1
        return 0
    fi

    bounded cmake --build "$bdir" --target help > "$dir/targets.txt" 2>/dev/null || true
    if grep -qw worldgen_probe "$dir/targets.txt"; then
        has_probe=1
    else
        CELL_PROBE=ABSENT
        printf 'probe target absent\n' > "$dir/probe.txt" || return 1
        echo "cell $CELL: probe target absent; continuing"
    fi

    rc=0
    bounded cmake --build "$bdir" --target initial_world_loading_perf_test --parallel 4 \
        > "$dir/build.log" 2>&1 || rc=$?
    if (( rc != 0 )); then
        CELL_PERF="$(build_failure_status "$rc")"
    else
        candidate="$(resolve_exe "$bdir/bin/initial_world_loading_perf_test")"
        if [[ -f "$candidate" ]]; then
            perf_exe="$candidate"
        else
            CELL_PERF=BUILD_FAILED
        fi
    fi

    if (( has_probe )); then
        rc=0
        bounded cmake --build "$bdir" --target worldgen_probe --parallel 4 \
            > "$dir/probe-build.log" 2>&1 || rc=$?
        if (( rc != 0 )); then
            CELL_PROBE="$(build_failure_status "$rc")"
        else
            candidate="$(resolve_exe "$bdir/bin/worldgen_probe")"
            if [[ -f "$candidate" ]]; then
                probe_exe="$candidate"
            else
                CELL_PROBE=BUILD_FAILED
            fi
        fi
    fi

    if [[ -n "$perf_exe" ]]; then
        exes+=("$perf_exe")
    fi
    if [[ -n "$probe_exe" ]]; then
        exes+=("$probe_exe")
    fi
    if (( ${#exes[@]} > 0 )); then
        bindir="$(dirname "${exes[0]}")"
        if ! stage_runtime "$dir" "$bindir" "${exes[@]}"; then
            finish_cell "$dir" EVIDENCE_FAILED || return 1
            return 0
        fi
        if [[ -n "$probe_exe" ]]; then
            run_probe "$dir" "$bdir" "$probe_exe"
        fi
        if [[ -n "$perf_exe" ]]; then
            run_perf "$dir" "$bdir" "$perf_exe"
        fi
    fi

    status=OK
    if [[ "$CELL_PERF" != OK ]]; then
        status="$CELL_PERF"
    elif [[ "$CELL_PROBE" != OK && "$CELL_PROBE" != ABSENT ]]; then
        status="$CELL_PROBE"
    fi
    finish_cell "$dir" "$status" || return 1
    return 0
}

write_summary() {
    local spread="n/a" first last delta
    first="${PREP_OF[0]:-}"
    last="${PREP_OF[5]:-}"
    if [[ -n "$first" && -n "$last" ]]; then
        if (( last > first )); then delta=$(( last - first )); else delta=$(( first - last )); fi
        spread="$delta"
    fi
    {
        echo "## UCRT64 package attribution"
        echo
        echo "Experiment status: $EXPERIMENT_STATUS"
        echo
        echo "| Cell | Label | Status | Probe | Perf | prep_ms | wall_seconds |"
        echo "| --- | --- | --- | --- | --- | --- | --- |"
        if [[ -f "$ROWS_FILE" ]]; then cat "$ROWS_FILE"; fi
        echo
        echo "cell0 versus cell5 prep_ms spread: $spread"
    } > "$SUMMARY_FILE"
}

# finalize: EXIT trap. Restores the baseline when a package swap is pending,
# records a cell that did not finish, and writes the summary from the rows
# that exist.
finalize() {
    local rc=$?
    trap - EXIT
    set +e
    if [[ -n "$CURRENT_CELL" ]]; then
        echo "cell $CURRENT_CELL did not finish; recording UNEXPECTED_EXIT" >&2
        CELL="$CURRENT_CELL"
        mkdir -p "$DIAG_DIR/cell-$CELL" 2>/dev/null
        finish_cell "$DIAG_DIR/cell-$CELL" UNEXPECTED_EXIT 2>/dev/null
        CURRENT_CELL=""
    fi
    if [[ "$EXPERIMENT_STATUS" == RUNNING ]]; then
        EXPERIMENT_STATUS=INCOMPLETE
    fi
    if (( DIRTY )); then
        begin_deadline
        if ! restore_to_baseline; then
            echo "final restore to baseline failed" >&2
            rc=1
        fi
    fi
    if ! write_summary; then
        echo "summary write failed" >&2
    fi
    if [[ -n "${GITHUB_STEP_SUMMARY:-}" && -f "$SUMMARY_FILE" ]]; then
        cat "$SUMMARY_FILE" >> "$GITHUB_STEP_SUMMARY" || true
    fi
    exit "$rc"
}

main() {
    local cell
    mkdir -p "$DIAG_DIR"
    for cell in $CELLS; do
        if [[ ! "$cell" =~ ^[0-5]$ ]]; then
            echo "invalid cell: $cell" >&2
            exit 2
        fi
    done
    trap finalize EXIT
    trap 'exit 143' TERM
    trap 'exit 130' INT
    : > "$ROWS_FILE"
    record_baseline
    if ! stage_baseline; then
        EXPERIMENT_STATUS=STAGING_FAILED
        echo "baseline staging failed; no intervention was attempted" >&2
        exit 0
    fi
    for cell in $CELLS; do
        run_cell "$cell"
        write_summary
        if (( ABORT_RUN )); then
            echo "restore did not reach the recorded baseline; stopping after cell $cell" >&2
            EXPERIMENT_STATUS=ABORTED
            break
        fi
    done
    if [[ "$EXPERIMENT_STATUS" == RUNNING ]]; then
        EXPERIMENT_STATUS=COMPLETE
    fi
}

main "$@"
