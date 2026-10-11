"""Experimental authenticated viewport transport. No Blender or GPU imports."""
import hashlib
import hmac
from functools import lru_cache
import json
import math
import mmap
import os
from pathlib import Path
import re
import stat
import struct
import tempfile

try:
    import numpy as _numpy
except ImportError:
    _numpy = None

HEADER_LIMIT = 1024 * 1024
PAYLOAD_LIMIT = 1280 * 720 * 9
PREFIX = struct.Struct('<4sIQ')
CAPACITY = PREFIX.size + 32 + HEADER_LIMIT + PAYLOAD_LIMIT
HEX32 = re.compile(r'[0-9a-f]{32}\Z')
HEX64 = re.compile(r'[0-9a-f]{64}\Z')


class Refusal(ValueError):
    pass


def validation_backend():
    return 'numpy/' + _numpy.__version__ if _numpy is not None else 'portable-python'


def require(value, message):
    if not value:
        raise Refusal(message)


def integer(value, low, high):
    return type(value) is int and low <= value <= high


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'),
                      ensure_ascii=True, allow_nan=False).encode('ascii')


def _object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, 'Duplicate JSON member')
        result[key] = value
    return result


def matrix(value):
    require(isinstance(value, list) and len(value) == 16, 'Matrix shape')
    require(all(type(x) in (int, float) and math.isfinite(x) and abs(x) <= 1e12
                for x in value), 'Matrix finite bounds')


def validate_state(s):
    require(type(s) is dict and set(s) == {
        'generation_id', 'manifest_sha256', 'scene_revision', 'camera_revision',
        'width', 'height', 'near_plane', 'far_plane', 'view', 'projection', 'locals'}, 'State members')
    require(type(s['generation_id']) is str and HEX32.fullmatch(s['generation_id']), 'Generation')
    require(type(s['manifest_sha256']) is str and HEX64.fullmatch(s['manifest_sha256']), 'Manifest')
    for field in ('scene_revision', 'camera_revision'):
        require(integer(s[field], 0, 2**53 - 1), 'Revision')
    require(integer(s['width'], 1, 1280) and integer(s['height'], 1, 720), 'Extent')
    require(type(s['near_plane']) in (int, float) and type(s['far_plane']) in (int, float)
            and math.isfinite(s['near_plane']) and math.isfinite(s['far_plane'])
            and 0.001 <= s['near_plane'] < s['far_plane'] <= 1e6, 'Clip planes')
    matrix(s['view'])
    matrix(s['projection'])
    require(type(s['locals']) is list and len(s['locals']) <= 1024, 'Local count')
    seen = set()
    for local in s['locals']:
        require(type(local) is dict and set(local) == {'instance_id', 'node_id', 'matrix'}, 'Local members')
        require(type(local['instance_id']) is str and re.fullmatch(r'[A-Za-z][A-Za-z0-9_.:-]{0,127}', local['instance_id']), 'Instance')
        node = local['node_id']
        require(type(node) is str and 0 < len(node) <= 128 and node.isascii()
                and all(32 <= ord(c) < 127 for c in node), 'Node')
        identity = (local['instance_id'], node)
        require(identity not in seen, 'Duplicate local')
        seen.add(identity)
        matrix(local['matrix'])
    require(len({item[0] for item in seen}) <= 64, 'Instance count')


def validate(header, payload):
    require(type(header) is dict, 'Header object')
    kind = header.get('kind')
    members = {'kind', 'session', 'sequence'}
    if kind in ('state', 'frame'):
        members.add('state')
    if kind == 'frame':
        members.add('planes_sha256')
    require(kind in ('state', 'frame', 'stop') and set(header) == members, 'Header members')
    require(type(header['session']) is str and HEX32.fullmatch(header['session']), 'Session')
    require(integer(header['sequence'], 1, 2**53 - 1), 'Sequence')
    if kind != 'stop':
        validate_state(header['state'])
    if kind != 'frame':
        require(not payload, 'Unexpected payload')
        return
    state = header['state']
    pixels = state['width'] * state['height']
    require(len(payload) == 9 * pixels, 'Plane sizes')
    require(header['planes_sha256'] == hashlib.sha256(payload).hexdigest(), 'Plane digest')
    if _numpy is not None:
        # Views retain the immutable bytes while vectorized checks avoid a
        # Python callback for each pixel. Keep the portable path equivalent.
        depth = _numpy.frombuffer(payload, dtype='<f4', count=pixels, offset=4*pixels)
        covered = _numpy.frombuffer(payload, dtype='u1', count=pixels, offset=8*pixels)
        rgba = _numpy.frombuffer(payload, dtype='u1', count=4*pixels)
        require(bool(_numpy.isfinite(depth).all() and (depth >= 0).all() and (depth <= 1).all()), 'Depth range')
        require(bool((covered <= 1).all() and _numpy.array_equal(covered, depth > 0)), 'Coverage depth')
        require(bool(_numpy.array_equal(rgba[3::4], covered * 255)), 'Coverage alpha')
        return
    for i, (depth,) in enumerate(struct.iter_unpack('<f', payload[4*pixels:8*pixels])):
        covered = payload[8*pixels+i]
        require(math.isfinite(depth) and 0 <= depth <= 1, 'Depth range')
        require(covered in (0, 1) and covered == (depth > 0), 'Coverage depth')
        require(payload[4*i+3] == 255 * covered, 'Coverage alpha')


def encode(header, payload, key):
    require(type(key) is bytes and len(key) == 32, 'Key length')
    validate(header, payload)
    raw = canonical(header)
    require(len(raw) <= HEADER_LIMIT and len(payload) <= PAYLOAD_LIMIT, 'Record bounds')
    prefix = PREFIX.pack(b'LVP1', len(raw), len(payload))
    signature = hmac.digest(key, prefix + raw + payload, 'sha256')
    return prefix + signature + raw + payload


def read_exact(stream, count):
    chunks = bytearray()
    while len(chunks) < count:
        part = stream.read(count - len(chunks))
        require(part, 'Truncated record')
        chunks.extend(part)
    return bytes(chunks)


def read_record(stream, key):
    require(type(key) is bytes and len(key) == 32, 'Key length')
    prefix = read_exact(stream, PREFIX.size)
    magic, header_size, payload_size = PREFIX.unpack(prefix)
    require(magic == b'LVP1' and 0 < header_size <= HEADER_LIMIT
            and payload_size <= PAYLOAD_LIMIT, 'Record bounds')
    signature = read_exact(stream, 32)
    raw = read_exact(stream, header_size)
    payload = read_exact(stream, payload_size)
    require(hmac.compare_digest(signature, hmac.digest(key, prefix + raw + payload, 'sha256')), 'Authentication')
    try:
        header = json.loads(raw, object_pairs_hook=_object,
                            parse_constant=lambda _: (_ for _ in ()).throw(Refusal('Nonfinite JSON')))
        validate(header, payload)
    except (TypeError, OverflowError, RecursionError, UnicodeError, json.JSONDecodeError) as error:
        raise Refusal('Invalid JSON') from error
    return header, payload


def write_record(stream, record):
    offset = 0
    while offset < len(record):
        count = stream.write(record[offset:])
        require(count is not None and count > 0, 'Short write')
        offset += count
    stream.flush()


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


def open_record_reader(path):
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


def read_record_file(path, limit):
    require(integer(limit, 0, CAPACITY), 'Record file limit')
    with open_record_reader(path) as stream:
        require(os.fstat(stream.fileno()).st_size <= limit, 'Record file size')
        raw = stream.read(limit + 1)
    require(len(raw) <= limit, 'Record file read bound')
    return raw


def replace_record(temporary, path):
    """Publish a complete record while shared readers retain the old file.

    Windows MoveFileExW (os.replace) does not supply POSIX replacement semantics.
    FileRenameInfoEx does; unsupported filesystems fail without altering the
    destination. This does not retry, remove the destination or write in place.
    """
    if os.name != 'nt':
        os.replace(temporary, path)
        return
    import ctypes
    from ctypes import wintypes as w
    temporary, path = safe_path(temporary), safe_path(path)
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


def atomic_write(path, value):
    path = safe_path(path)
    fd, temporary = tempfile.mkstemp(prefix='.pending-', dir=path.parent)
    try:
        with os.fdopen(fd, 'wb') as stream:
            stream.write(value)
            stream.flush()
        replace_record(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


class StateGate:
    """Admission order; immutable deep copies prevent caller mutation after validation."""
    def __init__(self, session):
        self.session = session
        self.last = None
        self.generations = {}

    def accept(self, header):
        validate(header, b'')
        require(header['kind'] in ('state', 'stop') and header['session'] == self.session, 'Session/kind')
        previous = self.last
        if previous:
            require(header['sequence'] > previous['sequence'], 'Replay')
            require(previous['kind'] != 'stop', 'Session closed')
        if header['kind'] == 'state':
            new = header['state']
            generation, manifest = new['generation_id'], new['manifest_sha256']
            known = self.generations.get(generation)
            require(known is None or known == manifest, 'Generation manifest changed')
            require(known is not None or len(self.generations) < 64, 'Generation session bound')
            if previous:
                old = previous['state']
                # Session-wide monotonicity survives latest-state coalescing,
                # including A -> B -> A when B never reaches the child.
                require(new['scene_revision'] >= old['scene_revision'] and
                        new['camera_revision'] >= old['camera_revision'], 'Revision rollback')
                camera = ('view', 'projection', 'width', 'height', 'near_plane', 'far_plane')
                require(all(old[k] == new[k] for k in camera) or
                        new['camera_revision'] > old['camera_revision'], 'Changed camera without revision')
                if generation == old['generation_id']:
                    require(new['locals'] == old['locals'] or new['scene_revision'] > old['scene_revision'],
                            'Changed transforms without revision')
                else:
                    require(new['scene_revision'] > old['scene_revision'], 'Generation barrier')
            self.generations[generation] = manifest
        self.last = json.loads(canonical(header))
        return self.last


def match_frame(frame, state):
    require(frame['kind'] == 'frame' and state['kind'] == 'state', 'Frame kind')
    require(all(frame[k] == state[k] for k in ('session', 'sequence', 'state')), 'Frame state join')


class Slots:
    def __init__(self, root, key, session, create=False):
        self.root, self.key, self.session = safe_path(root), key, session
        require(self.root.is_dir(), 'Session directory')
        if create:
            for index in range(2):
                path = self.root / f'frame-{index}.bin'
                with path.open('xb') as stream:
                    stream.truncate(CAPACITY)

    def _lease(self, index):
        path = safe_path(self.root / f'frame-{index}.lease')
        try:
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            return None
        os.close(fd)
        return path

    def publish(self, header, payload):
        require(header['kind'] == 'frame' and header['session'] == self.session, 'Slot session')
        record = encode(header, payload, self.key)
        for index in range(2):
            lease = self._lease(index)
            if lease is None:
                continue
            try:
                with safe_path(self.root / f'frame-{index}.bin').open('r+b') as stream:
                    require(os.fstat(stream.fileno()).st_size == CAPACITY, 'Slot capacity')
                    with mmap.mmap(stream.fileno(), CAPACITY) as mapped:
                        mapped[:len(record)] = record
                        mapped.flush()
                atomic_write(self.root / f'frame-{index}.json', canonical({
                    'length': len(record), 'sequence': header['sequence']}))
                return index
            finally:
                lease.unlink()
        return None

    def read(self, index):
        require(index in (0, 1), 'Slot index')
        lease = self._lease(index)
        if lease is None:
            return None
        try:
            descriptor = safe_path(self.root / f'frame-{index}.json')
            if not descriptor.exists():
                return None
            meta = json.loads(read_record_file(descriptor, 128), object_pairs_hook=_object)
            require(set(meta) == {'length', 'sequence'} and integer(meta['length'], 49, CAPACITY), 'Descriptor')
            with safe_path(self.root / f'frame-{index}.bin').open('rb') as stream:
                require(os.fstat(stream.fileno()).st_size == CAPACITY, 'Slot capacity')
                with mmap.mmap(stream.fileno(), CAPACITY, access=mmap.ACCESS_READ) as mapped:
                    import io
                    raw = io.BytesIO(mapped[:meta['length']])
                    header, payload = read_record(raw, self.key)
                    require(raw.tell() == meta['length'], 'Trailing slot bytes')
            require(header['kind'] == 'frame' and header['session'] == self.session
                    and header['sequence'] == meta['sequence'], 'Slot identity')
            return header, payload
        finally:
            lease.unlink()
