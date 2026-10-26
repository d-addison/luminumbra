#!/usr/bin/env python3
"""Verify daily Windows tools without configuring or building the project.

The previous jobs pinned CPython 3.14.8, MSVC ccache 4.14.1 and Mesa3D
26.1.7 (the latter two are downloaded and SHA256-verified by the workflow).
MSVC, Git, CMake, Ninja, 7-Zip and MSYS2 packages had no exact version pin;
The project minimum is CMake 3.20; these jobs require CMake/CTest 3.21
for --output-junit and PowerShell 7.4 for native-command error propagation.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys


def probe(name: str, *arguments: str) -> str:
    # Ignore empty/relative PATH entries, including the current worktree.
    search = os.pathsep.join(entry for entry in os.get_exec_path() if os.path.isabs(entry))
    executable = shutil.which(name, path=search)
    if not executable or not os.path.isabs(executable):
        raise ValueError(f"Missing required tool: {name}; install it for the runner service account")
    try:
        result = subprocess.run([executable, *arguments], capture_output=True, text=True,
                                encoding="utf-8", errors="replace", timeout=30, check=False)
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ValueError(f"Required tool {name} could not run: {type(error).__name__}") from error
    if result.returncode or not result.stdout.strip():
        raise ValueError(f"Required tool {name} failed its version/installation probe")
    return result.stdout.strip()


def verify(lane: str) -> None:
    if sys.platform != "win32":
        raise ValueError("The daily runner requires Windows x64")
    if struct.calcsize("P") != 8:
        raise ValueError("The daily runner requires 64-bit Python")
    if lane != "ucrt64" and sys.version_info[:3] != (3, 14, 8):
        raise ValueError("Required CPython version: 3.14.8 x64 (actions/setup-python)")
    for tool in ("cmake", "ctest"):
        output = probe(tool, "--version")
        version = re.search(r"version (\d+)\.(\d+)", output)
        if version is None or tuple(map(int, version.groups())) < (3, 21):
            raise ValueError(f"Required {tool} version: at least 3.21 (--output-junit)")
    probe("ninja", "--version")
    probe("git", "--version")
    if lane == "ucrt64":
        if os.environ.get("MSYSTEM") != "UCRT64":
            raise ValueError("Required MSYS2 environment: UCRT64")
        for tool in ("g++", "ccache", "python"):
            probe(tool, "--version")
        for package in ("numpy", "PIL"):
            __import__(package)
    elif lane == "msvc":
        if probe("ccache", "--version").splitlines()[0] != "ccache version 4.14.1":
            raise ValueError("Required MSVC ccache version: 4.14.1")
    elif lane == "native":
        probe("cl", "/?")
    else:
        # Git Bash must provide MSYS path conversion; the Windows WSL bash
        # launcher cannot run the unchanged bash contract steps.
        probe("bash", "--noprofile", "--norc", "-c", 'command -v cygpath && cygpath -w /')
        if lane != "aggregate":
            probe("7z", "i")
            output = probe("pwsh", "-NoProfile", "-Command", '$PSVersionTable.PSVersion.ToString()')
            version = re.fullmatch(r"(\d+)\.(\d+)\.\d+", output)
            if version is None or tuple(map(int, version.groups())) < (7, 4):
                raise ValueError("Required pwsh version: at least 7.4 for native-command errors")
        if lane == "host-msvc":
            location = os.environ.get("ProgramFiles(x86)", "")
            if not location or not os.path.isabs(location):
                raise ValueError("Missing required Visual Studio Installer location: ProgramFiles(x86)")
            tool = str(Path(location) / "Microsoft Visual Studio/Installer/vswhere.exe")
            try:
                probe(tool, "-latest", "-products", "*", "-requires",
                      "Microsoft.VisualStudio.Component.VC.Tools.x86.x64", "-find",
                      r"VC\Tools\MSVC\**\bin\Hostx64\x64\cl.exe")
            except ValueError as error:
                raise ValueError("Required MSVC x64 C++ tools and Visual Studio Installer/vswhere: " + str(error)) from error
        if lane == "aggregate" and not probe("python3", "--version").startswith("Python 3.14.8"):
            raise ValueError("Required aggregate python3 version: 3.14.8")
    print(f"Windows runner prerequisites ok: {lane}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lane", choices=("host-msvc", "host-ucrt64", "msvc", "ucrt64", "aggregate", "native"), required=True)
    args = parser.parse_args()
    try:
        verify(args.lane)
    except (ValueError, ImportError) as error:
        print(f"::error::{error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
