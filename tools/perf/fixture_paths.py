"""Portable containment for fixture and capture references."""

from __future__ import annotations

from pathlib import Path, PurePosixPath, PureWindowsPath


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
    """
    parts = reference_parts(reference)
    return contain_path(root, root.joinpath(*parts))
