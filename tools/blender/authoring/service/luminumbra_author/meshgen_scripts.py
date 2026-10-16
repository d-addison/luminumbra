"""Pinned game-source launch contract, independent of Blender and job supervision.

Use in a fresh worker interpreter. The permanent audit hook and import finder
bind project execution to verified source snapshots; this is an integrity guard
for trusted scripts, not a sandbox against hostile Python/native code.
"""
from __future__ import annotations

import ast
from contextlib import ExitStack
import errno
from functools import lru_cache
import importlib.abc
import importlib.machinery
import importlib.util
import os
from pathlib import Path, PurePosixPath, PureWindowsPath
import site
import stat
import sys
from types import CodeType
import weakref

from . import file_io
from .bootstrap import isolated_path
from .contracts import (MAX_DOCUMENT, Refusal, Toolchain, canonical, digest, require)

TRUSTED_MODULES = sys.stdlib_module_names | {"bpy", "bmesh", "mathutils"}
INDIRECT_NAMES = {"__import__", "exec", "eval", "compile"}
INDIRECT_ATTRIBUTES = {"import_module", "spec_from_file_location", "spec_from_loader",
    "module_from_spec", "exec_module", "load_module", "SourceFileLoader", "SourcelessFileLoader"}


def _posix_open(path, root_fd=None, directory=True):
    """Walk from an open directory, refusing links in every component."""
    require(os.open in os.supports_dir_fd and hasattr(os, "O_NOFOLLOW")
            and hasattr(os, "O_DIRECTORY"), "toolchain.script_path",
            "Platform cannot verify companion directory handles.")
    directory_flags = os.O_RDONLY | os.O_NOFOLLOW | os.O_DIRECTORY
    fd = os.dup(root_fd) if root_fd is not None else os.open(path.anchor, directory_flags)
    parts = path.parts if root_fd is not None else path.parts[1:]
    try:
        for index, part in enumerate(parts):
            flags = directory_flags if directory or index < len(parts) - 1 else (
                os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
            child = os.open(part, flags, dir_fd=fd)
            os.close(fd)
            fd = child
        return fd
    except BaseException:
        os.close(fd)
        raise


def _posix_identity(fd):
    info = os.fstat(fd)
    return info.st_dev, info.st_ino


@lru_cache(maxsize=1)
def _windows_path_api():
    import ctypes
    from ctypes import wintypes as w
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [w.LPCWSTR, w.DWORD, w.DWORD, w.LPVOID, w.DWORD, w.DWORD, w.HANDLE]
    kernel.CreateFileW.restype = w.HANDLE
    kernel.CloseHandle.argtypes, kernel.CloseHandle.restype = [w.HANDLE], w.BOOL
    kernel.GetFileInformationByHandleEx.argtypes = [w.HANDLE, ctypes.c_int, w.LPVOID, w.DWORD]
    kernel.GetFileInformationByHandleEx.restype = w.BOOL
    kernel.GetFinalPathNameByHandleW.argtypes = [w.HANDLE, w.LPWSTR, w.DWORD, w.DWORD]
    kernel.GetFinalPathNameByHandleW.restype = w.DWORD
    kernel.GetLongPathNameW.argtypes = [w.LPCWSTR, w.LPWSTR, w.DWORD]
    kernel.GetLongPathNameW.restype = w.DWORD
    return kernel


def _windows_path_key(name):
    path = PureWindowsPath(name)
    require(path.is_absolute() and len(path.drive) == 2 and path.drive[0].isascii()
            and path.drive[0].isalpha() and path.drive[1] == ':'
            and '..' not in path.parts and '\x00' not in name,
            "toolchain.script_path", "Unsupported companion path.")
    return path.as_posix().casefold()


def _windows_long_path(path):
    """Expand DOS short names without resolving away reparse components.

    Callers retain no-delete-sharing ancestors and check their reparse tags;
    spelling normalization must not turn a junction into an accepted target.
    """
    import ctypes
    name = str(path)
    _windows_path_key(name)
    kernel = _windows_path_api()
    size = kernel.GetLongPathNameW(name, None, 0)
    if not size:
        raise ctypes.WinError(ctypes.get_last_error())
    buffer = ctypes.create_unicode_buffer(size + 1)
    written = kernel.GetLongPathNameW(name, buffer, len(buffer))
    require(0 < written < len(buffer), "toolchain.script_path", "Cannot obtain long companion path.")
    return _windows_path_key(buffer.value)


def _windows_final_path(handle):
    import ctypes
    kernel = _windows_path_api()
    size = kernel.GetFinalPathNameByHandleW(handle, None, 0, 0)
    if not size:
        raise ctypes.WinError(ctypes.get_last_error())
    buffer = ctypes.create_unicode_buffer(size + 1)
    written = kernel.GetFinalPathNameByHandleW(handle, buffer, len(buffer), 0)
    require(0 < written < len(buffer), "toolchain.script_path", "Cannot obtain final companion handle path.")
    name = buffer.value
    # Accept only normalized DOS drive paths; UNC/device paths fail closed.
    require(name.startswith("\\\\?\\") and len(name) > 6 and name[5:7] == ":\\",
            "toolchain.script_path", "Unsupported companion handle path.")
    return _windows_path_key(name[4:])


def _windows_directory(path, share=3):
    import ctypes
    from ctypes import wintypes as w
    kernel = _windows_path_api()
    # READ_ATTRIBUTES; OPEN_EXISTING; BACKUP_SEMANTICS | OPEN_REPARSE_POINT.
    handle = kernel.CreateFileW(str(path), 0x80, share, None, 3, 0x02200000, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        class AttributeTag(ctypes.Structure):
            _fields_ = [("attributes", w.DWORD), ("tag", w.DWORD)]
        class Identity(ctypes.Structure):
            _fields_ = [("volume", ctypes.c_uint64), ("file_id", ctypes.c_ubyte * 16)]
        tag, identity = AttributeTag(), Identity()
        for kind, value in ((9, tag), (18, identity)):
            if not kernel.GetFileInformationByHandleEx(handle, kind, ctypes.byref(value), ctypes.sizeof(value)):
                raise ctypes.WinError(ctypes.get_last_error())
        require(not tag.attributes & 0x400, "path.symlink", "Companion directory is a reparse point.")
        require(tag.attributes & 0x10 and _windows_final_path(handle) == _windows_long_path(path),
                "toolchain.script_path", "Companion directory handle differs from its canonical path.")
        return handle, (identity.volume, bytes(identity.file_id))
    except BaseException:
        kernel.CloseHandle(handle)
        raise


def _path_refusal(error):
    rule = "path.symlink" if isinstance(error, ValueError) or error.errno in (errno.ELOOP, errno.ENOTDIR) else "toolchain.script_path"
    return Refusal(rule, "Cannot verify companion containment: " + str(error))


def validate_import_roots(record):
    roots = record.get("import_roots", [])
    require(isinstance(roots, list) and len(roots) <= 32 and all(
        isinstance(name, str) and bool(name) and not name.startswith("/")
        and ".." not in PurePosixPath(name).parts and "\\" not in name
        and ":" not in name and "\x00" not in name and str(PurePosixPath(name)) == name
        for name in roots), "toolchain.script_path", "Import roots must be canonical project-relative directories.")
    require(len(set(roots)) == len(roots), "toolchain.scripts", "Do not repeat import roots.")


def _close_inventory_handles(root_handles, active_reads):
    """Release active ancestor handles before the retained project root."""
    with ExitStack() as cleanup:
        cleanup.callback(root_handles.close)
        for stack in tuple(active_reads):
            cleanup.callback(stack.close)
        active_reads.clear()


class ScriptInventory:
    def __init__(self, project_root, record):
        self._root_handles = ExitStack()
        self._active_reads = set()
        # The callback owns resource state, never the inventory itself.
        self._close_root = weakref.finalize(self, _close_inventory_handles,
                                          self._root_handles, self._active_reads)
        try:
            self._initialize(project_root, record)
        except BaseException:
            self.close()
            raise

    def close(self):
        """Idempotently release retained root and any active ancestor handles."""
        self._close_root()

    def __enter__(self):
        require(self._close_root.alive, "toolchain.script_path", "Script inventory is closed.")
        return self

    def __exit__(self, exc_type, exc_value, traceback):
        self.close()

    def _initialize(self, project_root, record):
        validate_import_roots(record)
        self.root_alias = Path(project_root).absolute()
        self.root = Path(project_root).resolve(strict=True)
        try:
            if os.name == "posix":
                self.root_fd = _posix_open(self.root)
                self._root_handles.callback(os.close, self.root_fd)
                self.root_identity = _posix_identity(self.root_fd)
            elif os.name == "nt":
                # Retain identity without blocking replacements between reads.
                self.root_handle, self.root_identity = _windows_directory(self.root, share=7)
                self._root_handles.callback(_windows_path_api().CloseHandle, self.root_handle)
            else:
                raise Refusal("toolchain.script_path", "Unsupported companion handle verification platform.")
        except (OSError, ValueError) as error:
            if isinstance(error, Refusal):
                raise
            raise _path_refusal(error) from error
        self.pins = {record["path"]: record["sha256"], **record.get("modules", {})}
        for name in self.pins:
            require(isinstance(name, str) and bool(name) and not name.startswith("/")
                    and ".." not in PurePosixPath(name).parts and "\\" not in name
                    and ":" not in name and "\x00" not in name and str(PurePosixPath(name)) == name,
                    "toolchain.script_path", "Use canonical project-relative companion paths.")
        try:
            self.entry = self.canonical_path(self.root / record["path"])
        except OSError as error:
            raise Refusal("path.missing", "Entry does not exist inside its declared root.") from error
        require(self.entry.is_file(), "path.missing", "Entry requires a file inside its declared root.")
        # A module's import root is above its outermost pinned regular package.
        # Namespace packages require an explicit import_roots entry.
        roots = [self.entry.parent]
        for name in record.get("modules", {}):
            parent = (self.root / name).parent
            while parent != self.root and (parent / "__init__.py").relative_to(self.root).as_posix() in self.pins:
                parent = parent.parent
            roots.append(parent)
        for name in record.get("import_roots", []):
            path = self.root
            for part in PurePosixPath(name).parts:
                path /= part
                require(not path.is_symlink() and not (hasattr(path, "is_junction") and path.is_junction()),
                        "path.symlink", "Import roots cannot use symlinks or junctions.", name)
            require(path.is_dir(), "path.missing", "Import root does not exist.", name)
            require(any((self.root / member).is_relative_to(path) for member in self.pins),
                    "toolchain.scripts", "Companion roots must contain pinned source.", name)
            roots.append(path)
        self.roots = list(dict.fromkeys(roots))

    def canonical_path(self, path):
        """Accept root aliases, but never resolve away a companion reparse point."""
        path = Path(path).absolute()
        require(".." not in path.parts, "toolchain.script_path", "Companion traversal is unsupported.")
        for member in (path, *path.parents):
            # The declared project root may itself have been reached through a
            # symlink or a Windows short name. Only aliases of that root are OK.
            if member == self.root or member == self.root_alias:
                break
            require(not member.is_symlink() and not (hasattr(member, "is_junction") and member.is_junction()),
                    "path.symlink", "Companions cannot use symlinks or junctions.")
            if member.is_dir() and member.resolve(strict=True) == self.root:
                break  # A non-reparse alias, such as a Windows 8.3 spelling.
        path = path.resolve(strict=True)
        require(path.is_relative_to(self.root), "toolchain.script_path", "Companion escapes the project.")
        return path

    def source(self, path):
        path = self.canonical_path(path)
        name = path.relative_to(self.root).as_posix()
        require(name in self.pins, "config.pins_missing", "Project module has no pin.", name)
        data = self._read_source(name)
        require(digest(data) == self.pins[name], "toolchain.changed", "Pinned game source changed.", name)
        return data

    def _read_source(self, name):
        """Read the same handle whose containment and retained root were verified."""
        require(self._close_root.alive, "toolchain.script_path", "Script inventory is closed.")
        stack = ExitStack()
        self._active_reads.add(stack)
        try:
            with stack:
                if os.name == "posix":
                    current_root = _posix_open(self.root)
                    stack.callback(os.close, current_root)
                    require(_posix_identity(current_root) == self.root_identity,
                            "toolchain.script_path", "Canonical project root was replaced.")
                    fd = _posix_open(Path(name), self.root_fd, directory=False)
                    try:
                        require(stat.S_ISREG(os.fstat(fd).st_mode), "file.invalid", "Companion requires a regular file.")
                        stream = os.fdopen(fd, "rb")
                    except BaseException:
                        os.close(fd)
                        raise
                    stack.enter_context(stream)
                else:
                    # Hold every canonical directory without delete sharing:
                    # no ancestor can be renamed/replaced while the file opens.
                    path = Path(self.root.anchor)
                    for part in ("", *self.root.parts[1:], *PurePosixPath(name).parts[:-1]):
                        path /= part
                        handle, identity = _windows_directory(path)
                        stack.callback(_windows_path_api().CloseHandle, handle)
                        if path == self.root:
                            require(identity == self.root_identity, "toolchain.script_path",
                                    "Canonical project root was replaced.")
                    stream = stack.enter_context(file_io.open_regular_reader(self.root / name))
                    import msvcrt
                    final = _windows_final_path(msvcrt.get_osfhandle(stream.fileno()))
                    require(final == _windows_long_path(self.root / name),
                            "toolchain.script_path", "Opened companion escapes its canonical project path.")
                data = stream.read(MAX_DOCUMENT + 1)
                require(len(data) <= MAX_DOCUMENT, "file.limit", "File exceeds this profile's size limit.")
                return data
        except (OSError, ValueError) as error:
            if isinstance(error, Refusal):
                raise
            raise _path_refusal(error) from error
        finally:
            self._active_reads.discard(stack)

    def refuse_shadows(self):
        for name in sorted(TRUSTED_MODULES):
            require(importlib.machinery.PathFinder.find_spec(name, [str(p) for p in self.roots]) is None,
                    "toolchain.script_shadow", "Project import roots cannot shadow stdlib or Blender modules.", name)

    def verify(self):
        for name in self.pins:
            self.source(self.root / name)
        self.refuse_shadows()

    def local_import(self, module, roots):
        """Resolve all dotted segments on the same path as the runtime finder."""
        search = [str(p) for p in roots]
        for index, piece in enumerate(module.split(".")):
            if not piece:
                return False
            fullname = ".".join(module.split(".")[:index+1])
            spec = importlib.machinery.PathFinder.find_spec(fullname, search)
            if spec is None:
                return False
            if spec.origin:
                self.source(Path(spec.origin))
            search = spec.submodule_search_locations
            if search is None and index + 1 < len(module.split(".")):
                return False
        return True

    def diagnose(self):
        """Conservative early diagnostics only; runtime remains authoritative."""
        for name in self.pins:
            path = self.root / name
            tree = ast.parse(self.source(path), filename=name)
            for node in ast.walk(tree):
                require(not (isinstance(node, ast.Name) and isinstance(node.ctx, ast.Load) and node.id in INDIRECT_NAMES)
                        and not (isinstance(node, ast.Attribute) and node.attr in INDIRECT_ATTRIBUTES)
                        and not (isinstance(node, ast.alias) and node.name in INDIRECT_ATTRIBUTES),
                        "toolchain.script_inventory", "Indirect source loaders are unsupported by the pinned launch contract.", name)
                if not isinstance(node, (ast.Import, ast.ImportFrom)):
                    continue
                if isinstance(node, ast.Import):
                    modules, level = [alias.name for alias in node.names], 0
                else:
                    level = node.level
                    module = node.module or ""
                    modules = [module] + [module + ("." if module else "") + alias.name
                        for alias in node.names if alias.name != "*"]
                base = path.parent
                for _ in range(max(0, level - 1)):
                    base = base.parent
                search = [base] if level else self.roots
                for index, module in enumerate(modules):
                    if level and not module:
                        continue
                    found = self.local_import(module, search)
                    if not found and (isinstance(node, ast.Import) or index == 0):
                        require(not level and module.split(".")[0] in TRUSTED_MODULES,
                                "toolchain.script_inventory", "Unaccounted imported dependency.", module)


class PinnedLoader(importlib.abc.SourceLoader):
    def __init__(self, guard, path):
        self.guard, self.path = guard, Path(path)

    def create_module(self, spec):
        return None

    def get_filename(self, fullname):
        return str(self.path)

    def get_data(self, path):
        return self.guard.inventory.source(Path(path))

    def get_code(self, fullname):
        return self.guard.code(self.path)

    def exec_module(self, module):
        exec(self.guard.code(self.path), module.__dict__)


class ImportGuard(importlib.abc.MetaPathFinder):
    def __init__(self, inventory):
        self.inventory = inventory
        self.codes = {}

    def code(self, path):
        # Compile exactly the bytes just hashed, never cached .pyc or a later read.
        path = self.inventory.canonical_path(path)
        code = compile(self.inventory.source(path), str(path), "exec", dont_inherit=True)
        def register(item):
            self.codes[id(item)] = item
            for child in item.co_consts:
                if isinstance(child, CodeType):
                    register(child)
        register(code)
        return code

    def find_spec(self, fullname, path=None, target=None):
        spec = importlib.machinery.PathFinder.find_spec(fullname, path)
        if spec is None:
            return None
        origin = Path(spec.origin).absolute() if spec.origin else None
        locations = [Path(p).absolute() for p in spec.submodule_search_locations or ()]
        project = (origin and self.project_path(origin)) or any(self.project_path(p) for p in locations)
        if not project:
            return None
        origin = self.inventory.canonical_path(origin) if origin else None
        locations = [self.inventory.canonical_path(p) for p in locations]
        require(fullname.split(".")[0] not in TRUSTED_MODULES, "toolchain.script_shadow",
                "Project files cannot shadow trusted modules.", fullname)
        require(all(any(p.is_relative_to(root) for root in self.inventory.roots)
                    for p in ([origin] if origin else locations)),
                "toolchain.script_path", "Project import is outside the controlled roots.", fullname)
        if origin:
            require(isinstance(spec.loader, importlib.abc.SourceLoader),
                    "toolchain.script_inventory", "Project modules require pinned source loaders.", str(origin))
            # Validate the lexical origin before reading its canonical snapshot.
            self.inventory.source(Path(spec.origin))
            return importlib.util.spec_from_file_location(fullname, spec.origin,
                loader=PinnedLoader(self, spec.origin),
                submodule_search_locations=spec.submodule_search_locations)
        return spec  # Namespace directories execute no source.

    def project_path(self, filename):
        path = Path(filename)
        return path.absolute().is_relative_to(self.inventory.root) or path.resolve().is_relative_to(self.inventory.root)

    def check_spec(self, spec):
        """Use module locations and loader identity, never embedded co_filename."""
        if spec is None:
            return False
        locations = ([spec.origin] if spec.origin and spec.origin not in {"built-in", "frozen"} else []) + list(
            spec.submodule_search_locations or ())
        if not any(self.project_path(p) for p in locations):
            return False
        require(spec.origin and isinstance(spec.loader, importlib.abc.SourceLoader),
                "toolchain.script_inventory", "Project execution requires a pinned source loader.", spec.name)
        self.inventory.source(Path(spec.origin))
        return True

    def install_loader_checks(self):
        # Directly obtained standard loaders bypass meta_path. Bind their project
        # operations to the same snapshots; reject bytecode/native loaders before
        # they read or execute, independently of extension and embedded filenames.
        source_loader = importlib.machinery.SourceFileLoader
        original_source_code = source_loader.get_code
        def source_code(loader, fullname):
            if self.project_path(loader.path):
                return self.code(Path(loader.path).absolute())
            return original_source_code(loader, fullname)
        source_loader.get_code = source_code

        def checked(original):
            def operation(loader, *args, **kwargs):
                require(not self.project_path(loader.path), "toolchain.script_inventory",
                        "Project bytecode and native loaders are unsupported.", loader.path)
                return original(loader, *args, **kwargs)
            return operation
        bytecode_loader = importlib.machinery.SourcelessFileLoader
        bytecode_loader.get_code = checked(bytecode_loader.get_code)
        native_loader = importlib.machinery.ExtensionFileLoader
        native_loader.create_module = checked(native_loader.create_module)
        native_loader.exec_module = checked(native_loader.exec_module)

    def install_site_checks(self):
        original_directory, original_package = site.addsitedir, site.addpackage
        def directory(sitedir, *args, **kwargs):
            require(not self.project_path(sitedir), "toolchain.script_inventory",
                    "Project site-directory processing is unsupported.", str(sitedir))
            return original_directory(sitedir, *args, **kwargs)
        def package(sitedir, name, *args, **kwargs):
            require(not self.project_path(sitedir) and not self.project_path(Path(sitedir) / name),
                    "toolchain.script_inventory", "Project .pth processing is unsupported.", str(sitedir))
            return original_package(sitedir, name, *args, **kwargs)
        site.addsitedir, site.addpackage = directory, package

    def execution_frames(self, code):
        frame = sys._getframe(2)
        project_caller = False
        stdlib_import = False
        active_codes = set()
        while frame is not None:
            filename = frame.f_code.co_filename
            project_caller |= not filename.startswith("<") and self.project_path(filename)
            # Loader exec_module frames carry the actual module/spec even when
            # code has a forged external or synthetic filename.
            module = frame.f_locals.get("module")
            spec = getattr(module, "__spec__", None)
            loader = frame.f_locals.get("self")
            if spec is not None and loader is spec.loader:
                project_caller |= self.check_spec(spec)
                loaded_code = frame.f_locals.get("code")
                # Permit a real stdlib module import and its initialization-time
                # generated code only while no project frame intervenes. A
                # later project call into runpy/timeit/site gets no exemption.
                if ((loaded_code is code
                        or (not project_caller and id(loaded_code) in active_codes))
                        and spec.origin and spec.has_location
                        and isinstance(loader, (importlib.abc.SourceLoader,
                                                importlib.machinery.SourcelessFileLoader))
                        and not self.project_path(spec.origin)):
                    stdlib_import |= any(Path(spec.origin).absolute().is_relative_to(Path(p))
                                         for p in isolated_path(str(Path(__file__).parent.parent))[1:])
            active_codes.add(id(frame.f_code))
            frame = frame.f_back
        return project_caller, stdlib_import

    def audit(self, event, args):
        if event == "open" and isinstance(args[0], (str, bytes)):
            filename = args[0].decode() if isinstance(args[0], bytes) else args[0]
            require(not (Path(filename).suffix.lower() in {".pyc", ".pth"} and self.project_path(filename)),
                    "toolchain.script_inventory", "Project bytecode and .pth processing are unsupported.", filename)
        if event == "import" and args[1]:
            require(not self.project_path(args[1]), "toolchain.script_inventory",
                    "Project native module loading is unsupported.", args[1])
        if event == "compile" and isinstance(args[1], str) and not args[1].startswith("<") and self.project_path(args[1]):
            path = self.inventory.canonical_path(args[1])
            name = path.relative_to(self.inventory.root).as_posix()
            source = args[0].encode("utf-8") if isinstance(args[0], str) else args[0]
            require(name in self.inventory.pins and digest(source) == self.inventory.pins[name],
                    "toolchain.script_inventory", "Only pinned source bytes may be compiled from project paths.", name)
        if event != "exec":
            return
        code = args[0]
        project_caller, stdlib_import = self.execution_frames(code)
        if self.codes.get(id(code)) is code:
            return
        filename = code.co_filename
        project_code = not filename.startswith("<") and self.project_path(filename)
        require(not project_code and (not project_caller or stdlib_import), "toolchain.script_inventory",
                "Project code must execute through the pinned source loader.", filename)


def run_pinned_script(toolchain, project_root, role="finisher"):
    """Launch a manifest-selected entry in a fresh interpreter with a permanent guard."""
    root = Path(project_root).resolve()
    allowed = {Path(p).resolve() for p in isolated_path(str(Path(__file__).parent.parent))}
    require(sys.flags.isolated and sys.flags.no_site and sys.dont_write_bytecode
            and all(p and Path(p).resolve() in allowed and not Path(p).resolve().is_relative_to(root)
                    for p in sys.path),
            "toolchain.bootstrap", "Use the trusted launcher with -I -S -B and only service/stdlib paths.")
    toolchain.verify_scripts(project_root, role)
    with ScriptInventory(project_root, toolchain.scripts[role]) as inventory:
        inventory.verify()
        guard = ImportGuard(inventory)
        for module in list(sys.modules.values()):
            filename = getattr(module, "__file__", None)
            require(not filename or not guard.project_path(filename),
                    "toolchain.script_inventory", "Worker already loaded project code; use a fresh interpreter.")
        sys.dont_write_bytecode = True
        guard.install_loader_checks()
        guard.install_site_checks()
        sys.meta_path.insert(0, guard)
        sys.addaudithook(guard.audit)
        # Add project search roots only after runtime enforcement is installed.
        sys.path[:] = [str(p) for p in inventory.roots] + sys.path
        exec(guard.code(inventory.entry), {"__name__": "__main__", "__file__": str(inventory.entry),
                                         "__package__": None})


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--role", default="finisher")
    args = parser.parse_args()
    try:
        run_pinned_script(Toolchain(args.toolchain), args.project, args.role)
        return 0
    except (Refusal, ValueError, TypeError, OSError, ImportError, SyntaxError) as error:
        refusal = error if isinstance(error, Refusal) else Refusal("toolchain.script_inventory", str(error))
        print(canonical({"ok": False, "findings": [refusal.finding]}).decode())
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
