"""Pinned game-source launch contract, independent of Blender and job supervision.

Use in a fresh worker interpreter. The permanent audit hook and import finder
bind project execution to verified source snapshots; this is an integrity guard
for trusted scripts, not a sandbox against hostile Python/native code.
"""
from __future__ import annotations

import ast
import importlib.abc
import importlib.machinery
import importlib.util
from pathlib import Path, PurePosixPath
import site
import sys
from types import CodeType

from .bootstrap import isolated_path
from .contracts import (MAX_DOCUMENT, Refusal, Toolchain, canonical, digest,
                        read_bounded, relative_path, require)

TRUSTED_MODULES = sys.stdlib_module_names | {"bpy", "bmesh", "mathutils"}
INDIRECT_NAMES = {"__import__", "exec", "eval", "compile"}
INDIRECT_ATTRIBUTES = {"import_module", "spec_from_file_location", "spec_from_loader",
    "module_from_spec", "exec_module", "load_module", "SourceFileLoader", "SourcelessFileLoader"}


def validate_import_roots(record):
    roots = record.get("import_roots", [])
    require(isinstance(roots, list) and len(roots) <= 32 and all(
        isinstance(name, str) and bool(name) and not name.startswith("/")
        and ".." not in PurePosixPath(name).parts and "\\" not in name
        and ":" not in name and "\x00" not in name and str(PurePosixPath(name)) == name
        for name in roots), "toolchain.script_path", "Import roots must be canonical project-relative directories.")
    require(len(set(roots)) == len(roots), "toolchain.scripts", "Do not repeat import roots.")


class ScriptInventory:
    def __init__(self, project_root, record):
        validate_import_roots(record)
        self.root = Path(project_root).resolve(strict=True)
        self.pins = {record["path"]: record["sha256"], **record.get("modules", {})}
        self.entry = relative_path(self.root, record["path"])
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

    def source(self, path):
        path = Path(path)
        require(path.is_relative_to(self.root), "toolchain.script_path", "Companion escapes the project.")
        name = path.relative_to(self.root).as_posix()
        require(name in self.pins, "config.pins_missing", "Project module has no pin.", name)
        data = read_bounded(relative_path(self.root, name), MAX_DOCUMENT)
        require(digest(data) == self.pins[name], "toolchain.changed", "Pinned game source changed.", name)
        return data

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
        require(fullname.split(".")[0] not in TRUSTED_MODULES, "toolchain.script_shadow",
                "Project files cannot shadow trusted modules.", fullname)
        require(all(any(p.is_relative_to(root) for root in self.inventory.roots)
                    for p in ([origin] if origin else locations)),
                "toolchain.script_path", "Project import is outside the controlled roots.", fullname)
        if origin:
            require(isinstance(spec.loader, importlib.abc.SourceLoader),
                    "toolchain.script_inventory", "Project modules require pinned source loaders.", str(origin))
            # Check lexical origin too, to reject aliases and symlinks.
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
            path = Path(args[1]).absolute()
            require(path.is_relative_to(self.inventory.root), "toolchain.script_path", "Source alias is unsupported.")
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
    inventory = ScriptInventory(project_root, toolchain.scripts[role])
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
