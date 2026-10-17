#!/bin/bash
# Install with its Python policy OUTSIDE the checkout; see docs/development.md.
# No PATH search, checkout import, workflow input, or repository command is used.
# Bash startup precedes this script. This hook cannot enforce job admission;
# the external worker boundary must protect startup and enforce denial.
set -euo pipefail
case "$0" in
    /*) exec /usr/bin/python3 -I -B "${0%/*}/self_hosted_job_hook.py" ;;
    *) echo 'Self-hosted job denied: hook requires an absolute installed path' >&2; exit 1 ;;
esac
