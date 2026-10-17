"""Resolve a usable Bash for CI scripts without selecting the Windows WSL shim."""

import ntpath
import os
from pathlib import Path
import shutil
import subprocess
import sys


def _absolute_which(name):
    # Do not search the working directory or relative/empty PATH entries.
    search = os.pathsep.join(entry for entry in os.get_exec_path() if os.path.isabs(entry))
    found = shutil.which(name, path=search) if search else None
    return found if found and os.path.isabs(found) else None


def _windows_candidates(git, bash, msystem):
    candidates = [bash] if msystem and bash else []
    if git and ntpath.isabs(git):
        parent = ntpath.dirname(git)
        root = ntpath.dirname(parent)
        if ntpath.basename(root).lower() in ("mingw32", "mingw64", "usr"):
            root = ntpath.dirname(root)
        candidates.extend(ntpath.join(root, suffix) for suffix in ("bin/bash.exe", "usr/bin/bash.exe"))
    if bash:
        candidates.append(bash)
    unique = {}
    for candidate in candidates:
        unique.setdefault(ntpath.normcase(candidate), ntpath.normpath(candidate))
    return list(unique.values())


def bash_executable(override=None):
    """Return an absolute, probed executable or fail before any caller script runs.

    Native Windows uses Git Bash (or the active MSYS2 Bash); a working WSL
    distribution cannot substitute for native drive paths and process semantics.
    Explicit overrides undergo the same validation and never fall back.
    """
    windows = sys.platform == "win32"
    if override is not None:
        candidates = [override]
    else:
        bash = _absolute_which("bash")
        candidates = (_windows_candidates(_absolute_which("git"), bash, os.environ.get("MSYSTEM"))
                      if windows else [bash] if bash else [])
    script = '[[ -n "$BASH_VERSION" ]] || exit 1; '
    if windows:
        script += '[[ "$OSTYPE" == msys* ]] || exit 1; '
    script += "printf 'ci-bash-ready\\n'"
    environment = {key: value for key, value in os.environ.items()
                   if key.upper() not in ("BASH_ENV", "ENV", "SHELLOPTS", "BASHOPTS", "OSTYPE", "BASH_VERSION")
                   and not key.upper().startswith("BASH_FUNC_")}
    # At most three probes of two seconds each; never scan installations.
    for candidate in candidates:
        try:
            path = Path(candidate)
            if not path.is_absolute() or not path.is_file() or not os.access(path, os.X_OK):
                continue
            path = path.resolve(strict=True)
            result = subprocess.run(
                [str(path), "--noprofile", "--norc", "-e", "-c", script],
                env=environment, stdin=subprocess.DEVNULL, capture_output=True, timeout=2,
            )
            if result.returncode == 0 and result.stdout == b"ci-bash-ready\n" and not result.stderr:
                return str(path)
        except (OSError, RuntimeError, subprocess.TimeoutExpired):
            continue
    raise RuntimeError("A usable native Bash is required (install Git for Windows or MSYS2 on Windows)")
