#!/usr/bin/env python3
"""Scoped Windows render-child debugger; no installation or host configuration.

Only the launching thread owns debug events. All event handles are closed, the
child is killed/reaped on monitor failure or timeout, and exceptions are passed
back to Windows after capture. POSIX keeps subprocess.run's existing semantics.
"""
import ctypes as C
from contextlib import contextmanager
import hashlib
import os
from pathlib import Path
import platform
import stat
import struct
import sys
import subprocess
import tempfile
import time

# Per invocation: 512 loaded-image handles, 32 retained images/symbol candidates,
# 512 MiB total image bytes, 64 MiB dump, 1 MiB copy blocks. No full-memory dump.
MAX_MODULES = 512
MAX_FILES = 32
MAX_IMAGE_BYTES = 512 * 1024 * 1024
MAX_DUMP_BYTES = 64 * 1024 * 1024
BLOCK_BYTES = 1024 * 1024
RELEVANT_DLLS = {'opengl32.dll', 'libgallium_wgl.dll', 'libglapi.dll', 'libegl.dll',
                 'libglesv2.dll', 'vulkan-1.dll'}
U32, U64, PTR = C.c_uint32, C.c_uint64, C.c_void_p


class ExceptionRecord(C.Structure):
    _fields_ = [('code', U32), ('flags', U32), ('record', PTR), ('address', PTR),
                ('count', U32), ('information', U64 * 15)]


class ExceptionEvent(C.Structure):
    _fields_ = [('record', ExceptionRecord), ('first_chance', U32)]


class CreateEvent(C.Structure):
    _fields_ = [('file', PTR), ('process', PTR), ('thread', PTR), ('base', PTR),
                ('offset', U32), ('size', U32), ('start', PTR), ('name', PTR),
                ('unicode', C.c_uint16)]


class LoadEvent(C.Structure):
    _fields_ = [('file', PTR), ('base', PTR), ('offset', U32), ('size', U32),
                ('name', PTR), ('unicode', C.c_uint16)]


class UnloadEvent(C.Structure):
    _fields_ = [('base', PTR)]


class EventData(C.Union):
    _fields_ = [('exception', ExceptionEvent), ('create', CreateEvent),
                ('load', LoadEvent), ('unload', UnloadEvent),
                ('handle', PTR), ('exit_code', U32)]


class DebugEvent(C.Structure):
    _fields_ = [('kind', U32), ('pid', U32), ('tid', U32), ('data', EventData)]


class ExceptionPointers(C.Structure):
    _fields_ = [('record', PTR), ('context', PTR)]


class DumpException(C.Structure):
    _pack_ = 4  # MINIDUMP_EXCEPTION_INFORMATION uses the minidump 4-byte packing.
    _fields_ = [('tid', U32), ('pointers', PTR), ('client_pointers', C.c_int32)]


class CallbackInput(C.Structure):
    _pack_ = 4
    _fields_ = [('pid', U32), ('process', PTR), ('kind', U32)]


class IoCallback(C.Structure):
    _pack_ = 4
    _fields_ = [('handle', PTR), ('offset', U64), ('buffer', PTR), ('size', U32)]


def _bind(library, name, result, *arguments):
    function = getattr(library, name)
    function.restype, function.argtypes = result, list(arguments)
    return function


class WindowsDebugger:
    """Thin Win32 adapter, kept separate from the deterministic event policy."""
    def __init__(self):
        if C.sizeof(PTR) != 8 or platform.machine().lower() not in ('amd64', 'x86_64'):
            raise OSError('render crash capture requires a 64-bit Windows interpreter')
        self.kernel = C.WinDLL('kernel32', use_last_error=True, winmode=0x800)
        self.wait_event = _bind(self.kernel, 'WaitForDebugEvent', C.c_int32,
                                C.POINTER(DebugEvent), U32)
        self.continue_event = _bind(self.kernel, 'ContinueDebugEvent', C.c_int32,
                                    U32, U32, U32)
        self.detach_process = _bind(self.kernel, 'DebugActiveProcessStop', C.c_int32, U32)
        self.close_handle = _bind(self.kernel, 'CloseHandle', C.c_int32, PTR)
        self.file_name = _bind(self.kernel, 'GetFinalPathNameByHandleW', U32,
                               PTR, C.c_wchar_p, U32, U32)
        self.open_thread = _bind(self.kernel, 'OpenThread', PTR, U32, C.c_int32, U32)
        self.thread_context = _bind(self.kernel, 'GetThreadContext', C.c_int32, PTR, PTR)
        self.dump_library = C.WinDLL('dbghelp', use_last_error=True, winmode=0x800)
        self.write_dump = _bind(self.dump_library, 'MiniDumpWriteDump', C.c_int32,
                                 PTR, U32, PTR, U32, PTR, PTR, PTR)

    def wait(self, milliseconds):
        event = DebugEvent()
        if self.wait_event(C.byref(event), milliseconds):
            return event
        if C.get_last_error() == 121:  # ERROR_SEM_TIMEOUT
            return None
        raise C.WinError(C.get_last_error())

    def resume(self, event, status):
        if not self.continue_event(event.pid, event.tid, status):
            raise C.WinError(C.get_last_error())

    def detach(self, pid):
        if not self.detach_process(pid):
            raise C.WinError(C.get_last_error())

    def close(self, handle):
        if handle and not self.close_handle(handle):
            raise C.WinError(C.get_last_error())

    def image(self, handle):
        # Win32's maximum extended path is 32767 UTF-16 units, including NUL.
        name = C.create_unicode_buffer(32768)
        length = self.file_name(handle, name, len(name), 0)
        if not length or length >= len(name):
            self.close(handle)
            raise OSError('loaded image path unavailable or too long')
        import msvcrt
        try:
            descriptor = msvcrt.open_osfhandle(handle, os.O_RDONLY | os.O_BINARY)
        except OSError:
            self.close(handle)
            raise
        try:
            return Path(name.value), os.fdopen(descriptor, 'rb')
        except BaseException:
            os.close(descriptor)
            raise

    def dump(self, child, event, output, deadline):
        thread = self.open_thread(0x0048, False, event.tid)  # GET_CONTEXT | QUERY_INFORMATION
        if not thread:
            raise C.WinError(C.get_last_error())
        try:
            # AMD64 CONTEXT is 1232 bytes and must be aligned to 16 bytes.
            storage = C.create_string_buffer(1232 + 15)
            address = (C.addressof(storage) + 15) & ~15
            U32.from_address(address + 48).value = 0x0010000B  # CONTEXT_FULL
            if not self.thread_context(thread, address):
                raise C.WinError(C.get_last_error())
            registers = {name: U64.from_address(address + offset).value
                         for name, offset in zip(
                             ('rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi',
                              'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15', 'rip'),
                             range(120, 256, 8))}
            registers['eflags'] = U32.from_address(address + 68).value
            pointers = ExceptionPointers(C.addressof(event.data.exception.record), address)
            exception = DumpException(event.tid, C.addressof(pointers), False)
            self._dump_with_bounded_io(child, event.pid, output, exception, deadline)
            return registers
        finally:
            self.close(thread)

    def _dump_with_bounded_io(self, child, pid, output, exception, deadline):
        # DbgHelp I/O callbacks enforce the byte bound before each write, even
        # when a corrupt target reports very large memory regions. No giant
        # intermediate buffer or always-on dump is allocated.
        callback_type = C.WINFUNCTYPE(C.c_int32, PTR, PTR, PTR)
        failure = []
        io_started = False
        def callback(_parameter, incoming, outgoing):
            nonlocal io_started
            try:
                kind = C.cast(incoming, C.POINTER(CallbackInput)).contents.kind
                status = C.c_int32.from_address(outgoing)
                if kind == 11:  # IoStartCallback: S_FALSE selects callback I/O.
                    io_started = True
                    status.value = 1
                elif kind == 12:  # IoWriteAllCallback
                    data = IoCallback.from_address(incoming + C.sizeof(CallbackInput))
                    if not io_started or (data.size and not data.buffer):
                        raise OSError('minidump callback I/O unavailable')
                    if (data.size > MAX_DUMP_BYTES
                            or data.offset > MAX_DUMP_BYTES - data.size
                            or time.monotonic() >= deadline):
                        raise OSError('minidump byte/deadline bound reached')
                    output.seek(data.offset)
                    position = 0
                    while position < data.size:
                        if time.monotonic() >= deadline:
                            raise OSError('minidump deadline reached')
                        amount = min(BLOCK_BYTES, data.size - position)
                        payload = C.string_at(data.buffer + position, amount)
                        if output.write(payload) != len(payload):
                            raise OSError('partial minidump write')
                        position += amount
                    status.value = 0
                elif kind == 13:  # IoFinishCallback
                    status.value = 0
                elif kind == 6:  # CancelCallback
                    C.c_int32.from_address(outgoing).value = 1
                    C.c_int32.from_address(outgoing + 4).value = int(
                        bool(failure) or time.monotonic() >= deadline)
                return True
            except Exception as error:
                if not failure:
                    failure.append(str(error))
                C.c_int32.from_address(outgoing).value = -2147467259  # E_FAIL
                return False
        function = callback_type(callback)
        class CallbackInfo(C.Structure):
            _pack_ = 4
            _fields_ = [('routine', callback_type), ('parameter', PTR)]
        info = CallbackInfo(function, None)
        # MiniDumpNormal | MiniDumpWithUnloadedModules | MiniDumpWithThreadInfo.
        # No OS output handle: a DbgHelp implementation without callback
        # I/O must fail instead of bypassing the byte bound with native writes.
        if not self.write_dump(int(child._handle), pid, None, 0x1020,
                               C.byref(exception), None, C.byref(info)):
            raise OSError('MiniDumpWriteDump failed: ' + str(C.get_last_error()))
        if failure:
            raise OSError(failure[0])
        if not io_started:
            raise OSError('DbgHelp did not enable bounded callback I/O')
        output.flush()
        if not 0 < os.fstat(output.fileno()).st_size <= MAX_DUMP_BYTES:
            raise OSError('minidump is empty or exceeds byte bound')

class Evidence:
    def __init__(self, receipt, parent):
        self.receipt = receipt
        self.parent = Path(parent)
        self.directory = None
        self.images = {}
        self.bytes_left = MAX_IMAGE_BYTES
        self.files_left = MAX_FILES
        self.copy_deadline = float('inf')
        self.current_group = 0
        self.receipt['native_diagnostics'] = dict(status='monitoring', dump_status='missing',
                                                 directory=None, modules=[], files=[])
        self.data = self.receipt['native_diagnostics']

    def error(self, message):
        self.receipt['evidence_errors'].append('native diagnostics: ' + str(message))
        self.data['status'] = 'incomplete'

    def add(self, backend, handle, base):
        if not handle:
            self.data['unavailable_images'] = self.data.get('unavailable_images', 0) + 1
            return
        if len(self.images) >= MAX_MODULES:
            backend.close(handle)
            raise OSError('loaded module handle bound reached')
        name, stream = backend.image(handle)  # Consumes the native file handle, including on failure.
        old = self.images.pop(base, None)
        if old:
            old[1].close()
        self.images[base] = (name, stream)

    def unload(self, base):
        previous = self.images.pop(base, None)
        if previous:
            previous[1].close()

    def destination(self):
        if self.directory is None:
            self.directory = Path(tempfile.mkdtemp(prefix='native-failure-', dir=self.parent))
            self.data['directory'] = str(self.directory)
        return self.directory

    def copy(self, name, stream, role, expected_hash=None):
        metadata = os.fstat(stream.fileno())
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_size == 0:
            raise OSError('diagnostic input is not a regular file')
        if self.files_left == 0 or metadata.st_size > self.bytes_left:
            raise OSError('diagnostic image byte/file bound reached')
        group = self.destination() / f'image-{self.current_group:03d}'
        group.mkdir(exist_ok=True)
        target = group / name.name
        digest = hashlib.sha256()
        stream.seek(0)
        remaining = metadata.st_size
        created_identity = None
        try:
            with target.open('xb') as output:
                created = os.fstat(output.fileno())
                created_identity = (created.st_dev, created.st_ino)
                while remaining:
                    if time.monotonic() >= self.copy_deadline:
                        raise OSError('image retention deadline reached')
                    block = stream.read(min(BLOCK_BYTES, remaining))
                    if not block:
                        raise OSError('partial diagnostic input')
                    if output.write(block) != len(block):
                        raise OSError('partial diagnostic output')
                    remaining -= len(block)
                    digest.update(block)
                if stream.read(1) or _metadata(os.fstat(stream.fileno())) != _metadata(metadata):
                    raise OSError('diagnostic input changed during retention')
            if expected_hash is not None and digest.hexdigest() != expected_hash:
                raise OSError('retained executable does not match launch receipt')
        except BaseException:
            if created_identity is not None:
                try:
                    current = target.lstat()
                    if stat.S_ISREG(current.st_mode) and (current.st_dev, current.st_ino) == created_identity:
                        target.unlink()
                except FileNotFoundError:
                    pass
            raise
        self.files_left -= 1
        self.bytes_left -= metadata.st_size
        self.data['files'].append(dict(path=str(target), source=str(name), role=role,
                                       bytes=metadata.st_size, sha256=digest.hexdigest()))

    def retain_images(self):
        executable_retained = False
        symbol_sources = []
        self.copy_deadline = time.monotonic() + 5
        if self.data.get('unavailable_images'):
            self.error('some loaded image handles were unavailable')
        # Sorted load addresses give stable receipts. Never scan directories or
        # copy arbitrary system modules; all loaded modules still appear in dump.
        for index, (base, (name, stream)) in enumerate(sorted(self.images.items())):
            self.current_group = index
            executable = base == self.data.get('executable_base')
            self.data['modules'].append(dict(base=base, path=str(name)))
            if not executable and name.name.lower() not in RELEVANT_DLLS:
                continue
            try:
                expected = self.receipt.get('binary_sha256') if executable else None
                if executable and expected is None:
                    raise OSError('launch executable identity unavailable')
                self.copy(name, stream, 'executable' if executable else 'module', expected)
                executable_retained |= executable
                symbol_sources.append((index, name))
            except OSError as error:
                self.error(error)
        # Retain images before optional symbols so a large PDB cannot consume
        # the budget needed for the loaded Mesa image. At most MAX_FILES image
        # copies can succeed, each with exactly three adjacent symbol candidates.
        for index, name in symbol_sources:
            self.current_group = index
            for symbol in dict.fromkeys((name.with_suffix('.pdb'), name.with_suffix('.debug'),
                                         Path(str(name) + '.debug'))):
                try:
                    with open_symbol(symbol) as source:
                        self.copy(symbol, source, 'symbol_candidate')
                except FileNotFoundError:
                    continue
                except OSError as error:
                    self.error(error)
        if not executable_retained:
            self.error('matching launch executable was not retained')
        self.data['executable_retained'] = executable_retained

    def capture(self, backend, child, event, deadline):
        record = event.data.exception.record
        count = min(record.count, 15)
        self.data.update(status='capturing', exception_code=record.code,
                         exception_address=record.address, thread_id=event.tid,
                         exception_information=list(record.information[:count]))
        if record.code in (0xC0000005, 0xC0000006) and count >= 2:
            self.data.update(access_type={0: 'read', 1: 'write', 8: 'execute'}.get(
                record.information[0], 'unknown'), accessed_address=record.information[1])
        target = None
        created_identity = None
        try:
            target = self.destination() / 'exception.dmp'
            with target.open('x+b') as output:
                created = os.fstat(output.fileno())
                created_identity = (created.st_dev, created.st_ino)
                self.data['registers'] = backend.dump(child, event, output, deadline)
                output.flush()
                validate_dump_stream(output, record.code, event.tid)
            self.data.update(dump_status='retained', dump_path=str(target))
        except OSError as error:
            if target is not None:
                try:
                    remove_owned(target, created_identity)
                except OSError as cleanup_error:
                    self.error(cleanup_error)
            self.data['dump_status'] = 'failed'
            self.error(error)
        self.retain_images()
        if self.data['status'] != 'incomplete':
            self.data['status'] = 'retained'

    def close(self):
        for _name, stream in self.images.values():
            try:
                stream.close()
            except OSError as error:
                self.error('image handle close: ' + str(error))
        self.images.clear()


def monitored_run(command, *, cwd, env, stdout, stderr, timeout, check, receipt,
                  evidence_parent, backend, expected_exit=0):
    """No relaunch/fallback: a monitor error fails the invocation and kills child."""
    evidence = Evidence(receipt, evidence_parent)
    deadline = time.monotonic() + timeout
    child = None
    timed_out = False
    initial_breakpoint = True
    captured = False
    try:
        child = subprocess.Popen(command, cwd=cwd, env=env, stdout=stdout, stderr=stderr,
                                 creationflags=0x00000002)  # DEBUG_ONLY_THIS_PROCESS
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                evidence.data['status'] = 'timed_out'
                if not captured:
                    evidence.data['dump_status'] = 'not_captured_timeout'
                timed_out = True
                raise subprocess.TimeoutExpired(command, timeout)
            event = backend.wait(max(1, min(50, int(remaining * 1000))))
            if event is None:
                continue
            status = 0x00010002  # DBG_CONTINUE
            exited = event.kind == 5
            try:
                if event.kind == 3:  # CREATE_PROCESS_DEBUG_EVENT
                    # Win32 closes debug process/thread handles at their exit
                    # events; only image-file handles belong to this collector.
                    evidence.data['executable_base'] = event.data.create.base
                    evidence.add(backend, event.data.create.file, event.data.create.base)
                elif event.kind == 6:  # LOAD_DLL_DEBUG_EVENT
                    evidence.add(backend, event.data.load.file, event.data.load.base)
                elif event.kind == 7:  # UNLOAD_DLL_DEBUG_EVENT
                    evidence.unload(event.data.unload.base)
                elif event.kind == 1:
                    exception = event.data.exception
                    # Only consume the loader's first breakpoint. Application
                    # exceptions reach their own handlers on first chance.
                    if initial_breakpoint and exception.first_chance and exception.record.code == 0x80000003:
                        initial_breakpoint = False
                    else:
                        status = 0x80010001  # DBG_EXCEPTION_NOT_HANDLED
                        if not exception.first_chance and not captured:
                            captured = True
                            evidence.capture(backend, child, event, deadline)
                elif exited and evidence.data['status'] == 'monitoring':
                    evidence.data['status'] = 'normal_exit'
                    if event.data.exit_code != expected_exit:
                        evidence.data['status'] = 'nonzero_exit'
                        evidence.retain_images()
            finally:
                backend.resume(event, status)
            if exited:
                code = child.wait()
                if check and code:
                    raise subprocess.CalledProcessError(code, command)
                return subprocess.CompletedProcess(command, code)
    except OSError as error:
        evidence.error(error)
        raise
    finally:
        active_failure = sys.exc_info()[0] is not None
        cleanup_error = None
        if child is not None:
            # Terminate before detaching, so loss of the monitor cannot leave
            # an unobserved render child running. Detach releases pending events.
            if child.poll() is None:
                for operation in (child.kill, lambda: backend.detach(child.pid),
                                  lambda: child.wait(timeout=2)):
                    try:
                        operation()
                    except (OSError, subprocess.TimeoutExpired) as error:
                        evidence.error('child cleanup failed: ' + str(error))
                        cleanup_error = error
        try:
            if timed_out and not captured:
                evidence.retain_images()
        finally:
            evidence.close()
        if cleanup_error is not None and not active_failure:
            raise OSError('render debugger could not reap its child') from cleanup_error


def validate_dump(path, exception_code, thread_id):
    """Reject empty/partial dumps; require exception, module and thread streams."""
    with open_symbol(path) as source:
        validate_dump_stream(source, exception_code, thread_id)


def validate_dump_stream(source, exception_code, thread_id):
    # Read through the collector's owning handle, never a reopened output path.
    source.seek(0)
    size = os.fstat(source.fileno()).st_size
    header = source.read(32)
    if not 32 <= size <= MAX_DUMP_BYTES or len(header) != 32:
        raise OSError('missing, partial or oversized minidump')
    signature, version, streams, directory = struct.unpack_from('<4I', header)
    if signature != 0x504d444d or version & 0xffff != 0xa793:
        raise OSError('invalid minidump header')
    if not 3 <= streams <= 512 or directory > size - streams * 12:
        raise OSError('invalid minidump stream directory')
    source.seek(directory)
    entries = {}
    for _index in range(streams):
        entry = source.read(12)
        if len(entry) != 12:
            raise OSError('partial minidump directory')
        kind, length, offset = struct.unpack('<3I', entry)
        if kind == 0 and length == 0:
            continue  # DbgHelp may pad the directory with UnusedStream entries.
        if length == 0 or length > size or offset > size - length:
            raise OSError('partial minidump stream')
        if kind in entries:
            raise OSError('duplicate minidump stream')
        entries[kind] = (length, offset)
    if not {3, 4, 6}.issubset(entries):
        raise OSError('minidump lacks threads, modules or exception')
    exception_length, exception_offset = entries[6]
    if exception_length < 168:
        raise OSError('partial minidump exception')
    source.seek(exception_offset)
    exception = source.read(168)
    if len(exception) != 168:
        raise OSError('partial minidump exception')
    if (struct.unpack_from('<I', exception)[0] != thread_id
            or struct.unpack_from('<I', exception, 8)[0] != exception_code):
        raise OSError('minidump exception does not match this invocation')
    context_size, context_offset = struct.unpack_from('<2I', exception, 160)
    if context_size < 1232 or context_size > size or context_offset > size - context_size:
        raise OSError('minidump exception lacks AMD64 registers')
    threads_length, threads_offset = entries[3]
    source.seek(threads_offset)
    count_bytes = source.read(4)
    if len(count_bytes) != 4:
        raise OSError('partial minidump threads')
    count = struct.unpack('<I', count_bytes)[0]
    if not 1 <= count <= MAX_MODULES or threads_length < 4 + count * 48:
        raise OSError('invalid minidump thread count')
    stack_found = False
    for _index in range(count):
        thread = source.read(48)
        if len(thread) != 48:
            raise OSError('partial minidump thread')
        stack_size, stack_offset = struct.unpack_from('<2I', thread, 32)
        if struct.unpack_from('<I', thread)[0] == thread_id:
            stack_found = (0 < stack_size <= size and stack_offset <= size - stack_size)
    if not stack_found:
        raise OSError('minidump lacks faulting thread stack')
    modules_length, modules_offset = entries[4]
    source.seek(modules_offset)
    count_bytes = source.read(4)
    if len(count_bytes) != 4:
        raise OSError('partial minidump modules')
    modules = struct.unpack('<I', count_bytes)[0]
    if not 1 <= modules <= MAX_MODULES or modules_length < 4 + modules * 108:
        raise OSError('invalid minidump module count')

@contextmanager
def open_symbol(path):
    before = path.lstat()
    if (not stat.S_ISREG(before.st_mode) or before.st_nlink != 1
            or getattr(before, 'st_file_attributes', 0) & 0x400):
        raise OSError('symbol candidate must be a regular file with one link')
    flags = (os.O_RDONLY | getattr(os, 'O_NONBLOCK', 0)
             | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0))
    descriptor = os.open(path, flags)
    try:
        opened = os.fstat(descriptor)
        after = path.lstat()
        if (not stat.S_ISREG(opened.st_mode) or opened.st_nlink != 1
                or (opened.st_dev, opened.st_ino) != (before.st_dev, before.st_ino)
                or _metadata(before) != _metadata(after)):
            raise OSError('symbol candidate changed during open')
        stream = os.fdopen(descriptor, 'rb')
    except BaseException:
        os.close(descriptor)
        raise
    with stream:
        yield stream


def remove_owned(path, identity):
    if identity is None:
        return
    try:
        metadata = path.lstat()
        if stat.S_ISREG(metadata.st_mode) and (metadata.st_dev, metadata.st_ino) == identity:
            path.unlink()
    except FileNotFoundError:
        pass


def _metadata(value):
    return (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns)


def run(command, *, receipt, evidence_parent, expected_exit=0, **kwargs):
    if os.name != 'nt':
        return subprocess.run(command, **kwargs)
    backend = WindowsDebugger()  # Resolve all APIs before creating the child.
    return monitored_run(command, receipt=receipt, evidence_parent=evidence_parent,
                         backend=backend, expected_exit=expected_exit, **kwargs)
