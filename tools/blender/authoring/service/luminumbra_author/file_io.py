"""Shared standard-library file publication for service and viewport packages.

Readers hold a complete old snapshot while same-directory publication installs
an independently written replacement. Windows handles are non-inheritable and
explicitly support delete sharing; no retries or in-place writes hide failures.
This module is packaged byte-for-byte into the optional viewport extension.
"""
from functools import lru_cache
import os
from pathlib import Path
import stat


def require(value, message):
    if not value:
        raise ValueError(message)


def safe_path(path):
    path = Path(path).absolute()
    for part in (path, *path.parents):
        if not part.exists() and not part.is_symlink():
            continue
        info = part.lstat()
        require(not stat.S_ISLNK(info.st_mode) and not getattr(info, 'st_file_attributes', 0) & 0x400,
                'Reparse path')
    return path


@lru_cache(maxsize=1)
def _windows_reader_api():
    import ctypes
    from ctypes import wintypes as w
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.LPVOID, w.DWORD, w.DWORD, w.HANDLE]
    kernel.CreateFileW.restype = w.HANDLE
    kernel.GetFileInformationByHandleEx.argtypes = [w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD]
    kernel.GetFileInformationByHandleEx.restype = w.BOOL
    kernel.SetFileInformationByHandle.argtypes = [w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD]
    kernel.SetFileInformationByHandle.restype = w.BOOL
    kernel.CloseHandle.argtypes, kernel.CloseHandle.restype = [w.HANDLE], w.BOOL
    return kernel


def open_regular_reader(path):
    """Open one complete record without blocking another atomic publication.

    Windows CRT readers omit FILE_SHARE_DELETE. CreateFileW must explicitly
    permit replacement while this handle continues reading the old file.
    No retry, deadline extension, truncation or in-place write is involved.
    """
    path = safe_path(path)
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes as w
        import msvcrt
        kernel = _windows_reader_api()
        # GENERIC_READ; SHARE_READ | SHARE_WRITE | SHARE_DELETE; OPEN_EXISTING;
        # FILE_FLAG_OPEN_REPARSE_POINT. NULL security means no inherited handle.
        handle = kernel.CreateFileW(str(path), 0x80000000, 7, None, 3, 0x00200000, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            class AttributeTag(ctypes.Structure):
                _fields_ = [('attributes', w.DWORD), ('tag', w.DWORD)]
            info = AttributeTag()
            if not kernel.GetFileInformationByHandleEx(handle, 9, ctypes.byref(info), ctypes.sizeof(info)):
                raise ctypes.WinError(ctypes.get_last_error())
            require(not info.attributes & (0x400 | 0x10), 'Reparse or directory record handle')
            descriptor = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY | os.O_NOINHERIT)
        except BaseException:
            kernel.CloseHandle(handle)
            raise
        # Ownership transferred to the CRT descriptor; never CloseHandle again.
    else:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, 'O_NOFOLLOW', 0)
                             | getattr(os, 'O_NONBLOCK', 0))
    try:
        require(stat.S_ISREG(os.fstat(descriptor).st_mode), 'Record requires a regular file')
        return os.fdopen(descriptor, 'rb')
    except BaseException:
        os.close(descriptor)
        raise


def replace_file(temporary, path):
    """Publish a complete record while shared readers retain the old file.

    Windows MoveFileExW (os.replace) does not supply POSIX replacement semantics.
    FileRenameInfoEx does; unsupported filesystems fail without altering the
    destination. This does not retry, remove the destination or write in place.
    """
    temporary, path = safe_path(temporary), safe_path(path)
    require(temporary.parent == path.parent and temporary != path, 'Publication requires distinct same-directory files')
    require(stat.S_ISREG(temporary.lstat().st_mode), 'Publication source requires a regular file')
    if path.exists():
        require(stat.S_ISREG(path.lstat().st_mode), 'Publication destination requires a regular file')
    if os.name != 'nt':
        os.replace(temporary, path)
        return
    import ctypes
    from ctypes import wintypes as w
    name = str(path).encode('utf-16-le')
    require(0 < len(name) <= 65534, 'Record path length')
    class RenameInfo(ctypes.Structure):
        _fields_ = [('flags', w.DWORD), ('root', w.HANDLE),
                    ('length', w.DWORD), ('name', w.WCHAR * 1)]
    storage = ctypes.create_string_buffer(ctypes.sizeof(RenameInfo) + len(name) + 2)
    info = RenameInfo.from_buffer(storage)
    # FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS.
    info.flags, info.root, info.length = 3, None, len(name)
    ctypes.memmove(ctypes.addressof(storage) + RenameInfo.name.offset, name, len(name))
    kernel = _windows_reader_api()
    # DELETE | FILE_READ_ATTRIBUTES on the owned complete temporary file;
    # OPEN_REPARSE_POINT
    # prevents following a replaced leaf. Handles are never inherited.
    handle = kernel.CreateFileW(str(temporary), 0x10080, 7, None, 3, 0x00200000, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        class AttributeTag(ctypes.Structure):
            _fields_ = [('attributes', w.DWORD), ('tag', w.DWORD)]
        tag = AttributeTag()
        if not kernel.GetFileInformationByHandleEx(handle, 9, ctypes.byref(tag), ctypes.sizeof(tag)):
            raise ctypes.WinError(ctypes.get_last_error())
        require(not tag.attributes & (0x400 | 0x10), 'Reparse or directory publication handle')
        if not kernel.SetFileInformationByHandle(handle, 22, storage, len(storage)):
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(handle)
