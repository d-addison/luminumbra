"""Shared standard-library file publication for service and viewport packages.

Readers hold a complete old snapshot while same-directory publication installs
an independently written replacement. Windows handles are non-inheritable and
explicitly support delete sharing; no retries or in-place writes hide failures.
This module is packaged byte-for-byte into the optional viewport extension.
"""
from functools import lru_cache
from collections import namedtuple
import os
from pathlib import Path
import stat

FileIdentity = namedtuple('FileIdentity', 'volume file_id size modified changed attributes')


def require(value, message):
    if not value:
        raise ValueError(message)


def safe_path(path):
    path = Path(path).absolute()
    for part in (path, *path.parents):
        try:
            # Lease files are created/unlinked by the other participant. One
            # observation avoids an exists/lstat race and still inspects links.
            info = part.lstat()
        except FileNotFoundError:
            continue
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
    kernel.FlushFileBuffers.argtypes, kernel.FlushFileBuffers.restype = [w.HANDLE], w.BOOL
    kernel.CloseHandle.argtypes, kernel.CloseHandle.restype = [w.HANDLE], w.BOOL
    return kernel


def open_regular_reader(path):
    """Open one complete record without blocking another atomic publication.

    Windows CRT readers omit FILE_SHARE_DELETE. CreateFileW must explicitly
    permit replacement while this handle continues reading the old file.
    No retry, deadline extension, truncation or in-place write is involved.
    """
    path = safe_path(path)
    require(stat.S_ISREG(path.lstat().st_mode), 'Record requires a regular file')
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


def reader_identity(stream):
    """Return file identity, size, write time and metadata change time consistently.

    Windows path stat and descriptor stat can disagree about whether ctime is
    creation or change time. Query native handles for both comparisons instead.
    Times are opaque equality tokens (native ticks on Windows, ns on POSIX).
    """
    if os.name != 'nt':
        info = os.fstat(stream.fileno())
        require(stat.S_ISREG(info.st_mode), 'Identity requires a regular file')
        return FileIdentity(info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns, info.st_mode)
    import ctypes
    from ctypes import wintypes as w
    import msvcrt
    class Basic(ctypes.Structure):
        _fields_ = [('creation', ctypes.c_int64), ('access', ctypes.c_int64),
                    ('write', ctypes.c_int64), ('change', ctypes.c_int64), ('attributes', w.DWORD)]
    class Standard(ctypes.Structure):
        _fields_ = [('allocation', ctypes.c_int64), ('end', ctypes.c_int64),
                    ('links', w.DWORD), ('deleted', ctypes.c_ubyte), ('directory', ctypes.c_ubyte)]
    class Identity(ctypes.Structure):
        _fields_ = [('volume', ctypes.c_uint64), ('file_id', ctypes.c_ubyte * 16)]
    handle = msvcrt.get_osfhandle(stream.fileno())
    kernel = _windows_reader_api()
    basic, standard, identity = Basic(), Standard(), Identity()
    for kind, value in ((0, basic), (1, standard), (18, identity)):
        if not kernel.GetFileInformationByHandleEx(handle, kind, ctypes.byref(value), ctypes.sizeof(value)):
            raise ctypes.WinError(ctypes.get_last_error())
    require(not basic.attributes & (0x400 | 0x10) and not standard.directory and standard.end >= 0,
            'Identity requires a regular non-reparse file')
    return FileIdentity(identity.volume, bytes(identity.file_id), standard.end, basic.write, basic.change, basic.attributes)


def file_identity(path):
    with open_regular_reader(path) as stream:
        return reader_identity(stream)


def replace_file(temporary, path, *, durable=False, source_dir_fd=None, target_dir_fd=None,
                 allow_cross_directory=False):
    """Publish a complete record while shared readers retain the old file.

    Windows MoveFileExW (os.replace) does not supply POSIX replacement semantics.
    FileRenameInfoEx does; unsupported filesystems fail without altering the
    destination. This does not retry, remove the destination or write in place.
    With durable=True, Windows flushes the publication handle after renaming.
    Callers must still fsync complete temporary bytes before publication and
    fsync the containing directory afterwards on POSIX. Default callers retain
    their existing access rights and publication behavior.

    Descriptor-relative POSIX publication and Windows cross-directory staging
    require the caller to retain and verify every ancestor until durability is
    complete. These options support staging outside a candidate's seal.
    """
    if source_dir_fd is not None or target_dir_fd is not None:
        require(os.name == 'posix' and source_dir_fd is not None and target_dir_fd is not None
                and Path(temporary).name == str(temporary) and Path(path).name == str(path),
                'Relative publication requires two retained directories and leaf names')
        require(stat.S_ISREG(os.stat(temporary, dir_fd=source_dir_fd, follow_symlinks=False).st_mode),
                'Publication source requires a regular file')
        try:
            destination = os.stat(path, dir_fd=target_dir_fd, follow_symlinks=False)
        except FileNotFoundError:
            pass
        else:
            require(stat.S_ISREG(destination.st_mode), 'Publication destination requires a regular file')
        os.replace(temporary, path, src_dir_fd=source_dir_fd, dst_dir_fd=target_dir_fd)
        return
    temporary, path = safe_path(temporary), safe_path(path)
    require(allow_cross_directory or temporary.parent == path.parent,
            'Publication requires same-directory files unless verified cross-directory staging is enabled')
    require(temporary != path, 'Publication requires distinct files in verified directories')
    require(stat.S_ISREG(temporary.lstat().st_mode), 'Publication source requires a regular file')
    try:
        destination = path.lstat()
    except FileNotFoundError:
        pass
    else:
        require(stat.S_ISREG(destination.st_mode), 'Publication destination requires a regular file')
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
    # FlushFileBuffers requires GENERIC_WRITE; request it only for durability.
    access = 0x10080 | (0x40000000 if durable else 0)
    handle = kernel.CreateFileW(str(temporary), access, 7, None, 3, 0x00200000, None)
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
        if durable and not kernel.FlushFileBuffers(handle):
            # Publication has happened; propagate failure so recovery can
            # reconcile the complete record rather than claiming durability.
            raise ctypes.WinError(ctypes.get_last_error())
    finally:
        kernel.CloseHandle(handle)
