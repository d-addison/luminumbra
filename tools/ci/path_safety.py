"""Refuse Windows filesystem redirections before resolving or traversing paths."""

import os

WINDOWS = os.name == "nt"


def is_reparse_point(path):
    """Use lstat, never DirEntry's incomplete Windows stat metadata.

    All reparse tags are refused, including junctions and unknown tags. Older
    Python builds without file attributes fall back to the Win32 attribute API.
    Missing components are allowed for callers preparing fresh destinations.
    """
    try:
        info = os.lstat(path)
    except FileNotFoundError:
        return False
    attributes = getattr(info, "st_file_attributes", None)
    if attributes is None:
        junction = getattr(os.path, "isjunction", None)
        if junction is not None and junction(path):
            return True
        if not WINDOWS:
            return False
        import ctypes
        from ctypes import wintypes
        api = ctypes.WinDLL("kernel32", use_last_error=True)
        api.GetFileAttributesW.argtypes = (wintypes.LPCWSTR,)
        api.GetFileAttributesW.restype = wintypes.DWORD
        attributes = api.GetFileAttributesW(os.fsdecode(path))
        if attributes == 0xFFFFFFFF:
            raise ctypes.WinError(ctypes.get_last_error())
    return bool(attributes & 0x400)  # FILE_ATTRIBUTE_REPARSE_POINT


def reject_reparse_points(path):
    """Inspect original components, before realpath can erase a redirection."""
    if not WINDOWS:
        return
    current = os.fspath(path)
    # The loop is bounded by the number of components in the supplied path.
    while current:
        if is_reparse_point(current):
            raise OSError(f"reparse point is not allowed: {current}")
        parent = os.path.dirname(current)
        if parent == current:
            break
        current = parent
