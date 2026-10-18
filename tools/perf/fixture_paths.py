"""Portable containment for fixture and capture references."""

from __future__ import annotations

from contextlib import contextmanager
import io
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import stat


def _normalize_parts(parts: tuple[str, ...], reference: str) -> tuple[str, ...]:
    """Cancel dot components without allowing traversal above the root."""
    normalized: list[str] = []
    for part in parts:
        if part in ("", "."):
            continue
        if part == "..":
            if not normalized:
                raise ValueError(f"fixture reference escapes root: {reference}")
            normalized.pop()
        else:
            normalized.append(part)
    if not normalized:
        raise ValueError(f"fixture reference must name a root-relative path: {reference}")
    return tuple(normalized)


def reference_parts(reference: str) -> tuple[str, ...]:
    """Apply portable refusals, then normalize with the loader's host grammar.

    Root names (including drive-relative C:foo), root directories and escapes
    using either separator spelling are refused on every host. Accepted paths
    retain native spelling: a backslash is a literal character on POSIX and a
    separator on Windows, as in TerrainPresetLoader.
    """
    if not isinstance(reference, str) or not reference or "\0" in reference:
        raise ValueError("fixture reference must name a root-relative path")
    windows = PureWindowsPath(reference)
    if windows.drive or windows.root or PurePosixPath(reference).is_absolute():
        raise ValueError(f"fixture reference must be root-relative: {reference}")
    parts = _normalize_parts(tuple(reference.replace("\\", "/").split("/")), reference)
    normalized = PureWindowsPath("/".join(parts))
    if normalized.drive or normalized.root:
        raise ValueError(f"fixture reference must be root-relative: {reference}")
    return _normalize_parts(Path(reference).parts, reference)


def contain_path(root: Path, path: Path) -> Path:
    """Check an enumerated native path without reinterpreting its spelling.

    The caller supplies a path lexically below root. In particular, backslashes
    in POSIX directory entries are literal characters, not reference separators.
    """
    relative = path.relative_to(root)
    if ".." in relative.parts:
        raise ValueError(f"fixture reference escapes root: {relative}")
    root = root.resolve()
    contained = root / relative
    prefixes = [relative, *relative.parents]
    for prefix in reversed(prefixes[:-1]):
        path = root / prefix
        try:
            target = path.resolve()
        except (OSError, RuntimeError) as exc:
            raise ValueError(f"cannot resolve fixture reference: {relative}") from exc
        if not target.is_relative_to(root):
            raise ValueError(f"fixture reference resolves outside root: {relative}")
    return contained


def resolve_reference(root: Path, reference: str) -> Path:
    """Resolve a relative reference, refusing links that leave the selected root.

    Check every normalized prefix so a link cannot leave and then re-enter the
    root. Missing targets remain allowed, as in the native dependency loader.
    The root itself is caller-selected and may be a symlink.
    Use open_regular or read_bytes for reads; a returned pathname is not a handle.
    """
    parts = reference_parts(reference)
    return contain_path(root, root.joinpath(*parts))


def path_exists(path: Path) -> bool:
    """Keep missing-file fallbacks without suppressing other status errors."""
    try:
        path.stat()
    except FileNotFoundError:
        return False
    return True


def _open_posix(target: Path, flags: int) -> int:
    """Anchor every canonical component; replacements cannot introduce links."""
    # Search descriptors preserve ordinary traversal permissions. Platforms
    # without either flag use readable directory descriptors, still anchored.
    directory_flags = (getattr(os, "O_PATH", getattr(os, "O_SEARCH", os.O_RDONLY))
                       | os.O_DIRECTORY | os.O_NOFOLLOW)
    parent = os.open(target.anchor, directory_flags)
    try:
        for part in target.parts[1:-1]:
            child = os.open(part, directory_flags, dir_fd=parent)
            os.close(parent)
            parent = child
        return os.open(target.name, flags | os.O_NOFOLLOW, dir_fd=parent)
    finally:
        os.close(parent)


def _windows_path(name: str) -> Path:
    """Use the same DOS spelling for ordinary and extended handle paths."""
    if name.startswith("\\\\?\\UNC\\"):
        name = "\\\\" + name[8:]
    elif name.startswith("\\\\?\\"):
        name = name[4:]
    return Path(name)


def _windows_final_path(descriptor: int) -> Path:
    """Obtain the normalized long path from the owning file handle."""
    import ctypes
    from ctypes import wintypes
    import msvcrt

    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    final_path = kernel.GetFinalPathNameByHandleW
    final_path.argtypes = [wintypes.HANDLE, wintypes.LPWSTR, wintypes.DWORD, wintypes.DWORD]
    final_path.restype = wintypes.DWORD
    # Windows extended paths have at most 32767 characters plus a terminator.
    buffer = ctypes.create_unicode_buffer(32768)
    length = final_path(msvcrt.get_osfhandle(descriptor), buffer, len(buffer), 0)
    if not length:
        raise ctypes.WinError(ctypes.get_last_error())
    if length >= len(buffer):
        raise ValueError("opened fixture path exceeds platform bound")
    return _windows_path(buffer.value)


@contextmanager
def open_regular(root: Path, path: Path):
    """Read only a contained regular descriptor, never a checked pathname again.

    Contained links are resolved before opening. POSIX anchors the canonical
    path to directory descriptors and refuses replacement links at every step.
    Windows verifies the opened handle's long path, including ancestor swaps.
    Both branches compare file identity before permitting any bytes to be read.
    """
    relative = path.relative_to(root)
    root = root.resolve()
    checked = contain_path(root, root / relative)
    target = checked.resolve(strict=True)
    if not target.is_relative_to(root):
        raise ValueError(f"fixture reference resolves outside root: {path}")
    # Inspect the supplied name before opening its canonical target. A selected
    # root may have an alias (including a short Windows or linked POSIX name);
    # canonicalization must not bypass metadata checks on that input.
    expected = path.stat()
    refusal = f"fixture reference is not a regular file: {path}"
    if not stat.S_ISREG(expected.st_mode):
        raise ValueError(refusal)
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NONBLOCK", 0)
    descriptor = _open_posix(target, flags) if os.name == "posix" else os.open(target, flags)
    try:
        opened = os.fstat(descriptor)
        if not stat.S_ISREG(opened.st_mode):
            raise ValueError(refusal)
        if os.name == "nt" and _windows_final_path(descriptor) != _windows_path(str(target)):
            raise ValueError(f"opened fixture reference changed target: {path}")
        if (opened.st_dev, opened.st_ino) != (expected.st_dev, expected.st_ino):
            raise ValueError(f"opened fixture reference changed target: {path}")
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            yield stream
    finally:
        os.close(descriptor)


def read_bytes(root: Path, path: Path, *, limit: int | None = None) -> bytes:
    """Read through the verified descriptor, enforcing optional size bounds."""
    with open_regular(root, path) as stream:
        if limit is not None and os.fstat(stream.fileno()).st_size > limit:
            raise ValueError(f"oversized fixture file: {path.name}")
        data = stream.read() if limit is None else stream.read(limit + 1)
        if limit is not None and len(data) > limit:
            raise ValueError(f"oversized fixture file: {path.name}")
        return data


def decode_text(data: bytes, *, encoding: str = "utf-8") -> str:
    """Match text-mode decoding, including universal newline translation."""
    return io.IncrementalNewlineDecoder(None, translate=True).decode(data.decode(encoding), final=True)


def read_text(root: Path, path: Path, *, encoding: str = "utf-8",
              limit: int | None = None) -> str:
    """Read text through the verified descriptor with an optional byte bound."""
    return decode_text(read_bytes(root, path, limit=limit), encoding=encoding)
