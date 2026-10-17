"""Resolve a usable Bash for CI scripts without selecting the Windows WSL shim."""

import ntpath
import os
from contextvars import ContextVar
from pathlib import Path
import shutil
import subprocess
import sys


_discovery_observations = ContextVar("bash_discovery_observations", default=None)


def _observation(label, value, preview_units=160):
    # Slice before escaping: 160 input units / 640 displayed ASCII characters
    # per value (1024 / 4096 for PATH). Never format whole probe output.
    if isinstance(value, (str, bytes)):
        preview = ascii(value[:preview_units])
        truncated = len(value) > preview_units or len(preview) > 4 * preview_units
        return f"{label}={preview[:4 * preview_units]} (length={len(value)}, truncated={truncated})"
    return f"{label}={value!r}"


def _failure_context():
    # Fixed allowlist only; no shell hooks, general environment, or exception
    # messages (which can contain arbitrary caller data). No discovery is repeated.
    values = [
        ("interpreter", sys.executable), ("python", sys.version),
        ("sys.platform", sys.platform), ("os.name", os.name),
        ("path_module", os.path.__name__), ("pathsep", os.pathsep),
    ]
    values.extend((name, os.environ.get(name)) for name in (
        "PATH", "PATHEXT", "MSYSTEM", "MSYS2_ARG_CONV_EXCL", "MSYS_NO_PATHCONV",
        "MSYS2_ENV_CONV_EXCL", "MSYS2_PATH_TYPE",
    ))
    return [_observation(label, value, 1024 if label == "PATH" else 160) for label, value in values]


def _absolute_which(name):
    # Do not search the working directory or relative/empty PATH entries.
    search = os.pathsep.join(entry for entry in os.get_exec_path() if os.path.isabs(entry))
    found = shutil.which(name, path=search) if search else None
    selected = found if found and os.path.isabs(found) else None
    observations = _discovery_observations.get()
    if observations is not None:
        observations.extend((_observation(f"discovery.{name}.absolute_PATH", search),
                             _observation(f"discovery.{name}.which", found)))
    return selected


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
    Failure details contain bounded observations; probe bytes are never normalized.
    """
    windows = sys.platform == "win32"
    observations = [f"selection={'override' if override is not None else 'discovery'}"]
    if override is not None:
        candidates = [override]
    else:
        # Observe the lookup's actual inputs/results without repeating it or
        # changing its call contract. The context is isolated and always reset.
        token = _discovery_observations.set(observations)
        try:
            bash = _absolute_which("bash")
            git = _absolute_which("git") if windows else None
        finally:
            _discovery_observations.reset(token)
        observations.extend((_observation("discovery.bash", bash), _observation("discovery.git", git)))
        candidates = (_windows_candidates(git, bash, os.environ.get("MSYSTEM"))
                      if windows else [bash] if bash else [])
    observations.append(f"candidates={len(candidates)}; discovery uses absolute PATH entries only")
    script = '[[ -n "$BASH_VERSION" ]] || exit 1; '
    if windows:
        script += '[[ "$OSTYPE" == msys* ]] || exit 1; '
    script += "printf 'ci-bash-ready\\n'"
    environment = {key: value for key, value in os.environ.items()
                   if key.upper() not in ("BASH_ENV", "ENV", "SHELLOPTS", "BASHOPTS", "OSTYPE", "BASH_VERSION")
                   and not key.upper().startswith("BASH_FUNC_")}
    # At most three probes of two seconds each; never scan installations.
    for index, candidate in enumerate(candidates):
        label = f"candidate[{index}]"
        # Only the existing (at most three) candidates are observed. These records
        # and the fixed context above keep the complete failure below 24 KiB.
        stage = "path"
        try:
            path = Path(candidate)
            observations.append(_observation(label, str(path)))
            stage = "is_absolute"
            absolute = path.is_absolute()
            observations.append(f"{label}.is_absolute={absolute}")
            if not absolute:
                continue
            stage = "is_file"
            regular = path.is_file()
            observations.append(f"{label}.is_file={regular}")
            if not regular:
                continue
            stage = "access"
            executable = os.access(path, os.X_OK)
            observations.append(f"{label}.executable={executable}")
            if not executable:
                continue
            stage = "resolve"
            path = path.resolve(strict=True)
            observations.append(_observation(f"{label}.resolved", str(path)))
            stage = "probe"
            result = subprocess.run(
                [str(path), "--noprofile", "--norc", "-e", "-c", script],
                env=environment, stdin=subprocess.DEVNULL, capture_output=True, timeout=2,
            )
            if result.returncode == 0 and result.stdout == b"ci-bash-ready\n" and not result.stderr:
                return str(path)
            observations.extend((f"{label}.probe.returncode={result.returncode}",
                                 _observation(f"{label}.probe.stdout", result.stdout),
                                 _observation(f"{label}.probe.stderr", result.stderr)))
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            observations.append(f"{label}.{stage}.exception={type(error).__name__}")
            if isinstance(error, OSError):
                observations.extend((f"{label}.errno={error.errno}",
                                     f"{label}.winerror={getattr(error, 'winerror', None)}"))
            if isinstance(error, subprocess.TimeoutExpired):
                observations.extend((_observation(f"{label}.probe.stdout", error.output),
                                     _observation(f"{label}.probe.stderr", error.stderr)))
            continue
    raise RuntimeError("A usable native Bash is required (install Git for Windows or MSYS2 on Windows)\n"
                       + "\n".join(_failure_context() + observations))
