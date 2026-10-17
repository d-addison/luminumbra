#!/usr/bin/env bash
# The dedicated Linux host is provisioned by its operator, without sudo.
set -euo pipefail

# Refuse a subdirectory or another repository before removing checkout leftovers.
workspace=$(git -C "$GITHUB_WORKSPACE" rev-parse --show-toplevel)
if [[ "$workspace" != "$GITHUB_WORKSPACE" || "$PWD" != "$workspace" ]]; then
    echo '::error::Expected the checkout root as the working directory' >&2
    exit 1
fi
# RUNNER_TEMP and the checkout must belong to the same dedicated instance.
# The metadata hook independently checks root-owned configuration. Execution
# admission requires an external worker boundary; see docs/development.md.
if [[ "$RUNNER_TEMP" != */_temp || "$workspace" != "${RUNNER_TEMP%/_temp}/luminumbra/luminumbra" ||
      -L "$workspace" || -L "$RUNNER_TEMP" || ! -O "$workspace" || ! -O "$RUNNER_TEMP" ]]; then
    echo '::error::Expected runner-owned checkout and temporary directories' >&2
    exit 1
fi

missing=()
# Fixed provisioning inventory: check every package and executable, accumulating
# failures so the operator receives the complete missing list in one attempt.
packages=(
    build-essential ccache cmake extra-cmake-modules libfreetype6-dev
    libgl1-mesa-dev libglu1-mesa-dev libwayland-dev libxkbcommon-dev
    ninja-build pkg-config python3-numpy python3-pil python3-jsonschema
    wayland-protocols xauth xorg-dev xvfb clang clang-tidy clang-format-18
)
for package in "${packages[@]}"; do
    if [[ "$(dpkg-query -W -f='${Status}' "$package" 2>/dev/null)" != 'install ok installed' ]]; then
        missing+=("package:$package")
    fi
done
for tool in dpkg-query git gcc g++ make ccache cmake ctest ninja pkg-config python3 Xvfb xvfb-run xauth clang clang++ clang-tidy clang-format-18; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        missing+=("command:$tool")
    fi
done
if (( ${#missing[@]} )); then
    printf '::error::Missing self-hosted dependencies: %s\n' "${missing[*]}" >&2
    exit 1
fi

# Refuse missing dependencies before deleting checkout leftovers. The required
# external boundary must admit the job before checkout; this clears the stable
# compiler cache before any Actions restore can run.
git -C "$workspace" clean -ffdx -- .

# Each runner instance owns RUNNER_TEMP; each job also gets a unique directory.
# The runner removes RUNNER_TEMP at job completion, including cancelled jobs.
umask 077
job_temp=$(mktemp -d "$RUNNER_TEMP/lum-ci.XXXXXXXX")
job_home="$job_temp/home"
mkdir -p "$workspace/.cache/ccache" "$job_home/.cache" "$job_home/.config/ccache" "$job_temp/tmp" "$job_temp/runtime" "$job_temp/bin"
: > "$job_home/.config/ccache/ccache.conf"
# The host only needs the versioned formatter; keep the hosted install untouched.
ln -s "$(command -v clang-format-18)" "$job_temp/bin/clang-format"
# Manual capture steps use python; the provisioned package supplies python3.
ln -s "$(command -v python3)" "$job_temp/bin/python"
{
    echo "HOME=$job_home"
    echo "XDG_CACHE_HOME=$job_home/.cache"
    echo "XDG_CONFIG_HOME=$job_home/.config"
    echo "XDG_RUNTIME_DIR=$job_temp/runtime"
    echo "TMPDIR=$job_temp/tmp"
    echo "TMP=$job_temp/tmp"
    echo "TEMP=$job_temp/tmp"
    echo "CCACHE_DIR=$workspace/.cache/ccache"
    echo "CCACHE_TEMPDIR=$job_temp/tmp"
    echo "CCACHE_CONFIGPATH=$job_home/.config/ccache/ccache.conf"
    echo 'PYTHONNOUSERSITE=1'
} >> "$GITHUB_ENV"
echo "$job_temp/bin" >> "$GITHUB_PATH"
