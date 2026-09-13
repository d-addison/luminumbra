"""Complete installed static-preview identity checks, without Blender imports."""
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import stat
import time

from .viewport.protocol import canonical, require, safe_path


SOURCE_INPUTS = 'share/luminumbra-static/source-inputs.txt'
SHADERS = {'share/luminumbra-static/res/shaders/' + name for name in (
    'static_asset.vert', 'static_asset.frag', 'lighting_pass.vert',
    'lighting_pass.frag', 'config_constants.gen.glsl')}
MAX_FILES = 4096
MAX_BYTES = 2 * 1024 ** 3


def check_deadline(deadline, cancelled=None):
    if cancelled is not None and cancelled.is_set():
        raise InterruptedError('Viewport client closing')
    if time.monotonic() >= deadline:
        raise TimeoutError('Viewport identity deadline')


def file_hash(path, deadline, cancelled=None):
    path = safe_path(path)
    require(stat.S_ISREG(path.stat().st_mode), 'Identity requires a regular file')
    require(path.stat().st_size <= MAX_BYTES, 'Identity file size bound')
    result = hashlib.sha256()
    with path.open('rb') as stream:
        while True:
            check_deadline(deadline, cancelled)
            data = stream.read(1024 * 1024)
            if not data:
                return result.hexdigest()
            result.update(data)


def relative_path(value):
    require(type(value) is str and 0 < len(value) <= 1024 and '\\' not in value and ':' not in value,
            'Manifest repository-relative path')
    path = PurePosixPath(value)
    require(not path.is_absolute() and path.parts and '.' not in path.parts and '..' not in path.parts
            and path.as_posix() == value, 'Manifest canonical relative path')
    return path


def _object(pairs):
    value = {}
    for key, item in pairs:
        require(key not in value, 'Duplicate manifest member')
        value[key] = item
    return value


def renderer_module(files):
    names = [name for name in files if re.fullmatch(
        r'(bin|lib)/(lib)?luminumbra_render_static\.(dll|so|dylib)', name)]
    require(len(names) == 1, 'Exactly one installed renderer module is required')
    return names[0]


def verify_installation(manifest, host, deadline, *, expected_manifest=None, cancelled=None):
    """Manifest lives directly in SDK root and is the only self-excluded file."""
    manifest, host = safe_path(manifest), safe_path(host)
    require(manifest.is_file() and manifest.stat().st_size <= 1024 * 1024, 'Manifest size/file')
    identity = file_hash(manifest, deadline, cancelled)
    require(expected_manifest is None or expected_manifest == identity, 'Installation manifest changed')
    data = json.loads(manifest.read_bytes(), object_pairs_hook=_object)
    require(type(data) is dict and set(data) == {
        'schema', 'source_commit', 'source_dirty', 'source_input_sha256', 'executable', 'files',
        'identity_receipt_sha256'}, 'Installation manifest members')
    require(data['schema'] == 'luminumbra.static_preview.installation.v1', 'Installation manifest schema')
    require(type(data['source_commit']) is str and re.fullmatch('[0-9a-f]{40}', data['source_commit']), 'Source commit')
    require(data['source_dirty'] is False, 'Installation source must be clean')
    require(type(data['identity_receipt_sha256']) is str
            and re.fullmatch('[0-9a-f]{64}', data['identity_receipt_sha256']), 'Installation receipt identity')
    require(type(data['source_input_sha256']) is str and re.fullmatch('[0-9a-f]{64}', data['source_input_sha256']),
            'Source input identity')
    root = manifest.parent
    executable = relative_path(data['executable'])
    require(executable.parts[0] == 'bin' and root / executable == host, 'Manifest executable must identify actual host')
    files = data['files']
    require(type(files) is dict and 0 < len(files) <= MAX_FILES, 'Installation file count')
    names = set()
    for name, digest in files.items():
        relative_path(name)
        require(name.casefold() not in names, 'Case-aliased installation path')
        names.add(name.casefold())
        require(type(digest) is str and re.fullmatch('[0-9a-f]{64}', digest), 'Installation file digest')
    require(data['executable'] in files and SOURCE_INPUTS in files and SHADERS <= files.keys(),
            'Host, source-inputs and all five shaders are required')
    require(files[SOURCE_INPUTS] == data['source_input_sha256'], 'Source input digest mismatch')
    renderer_module(files)
    actual, total = set(), 0
    for path in root.rglob('*'):
        check_deadline(deadline, cancelled)
        safe_path(path)
        if path == manifest or path.is_dir():
            continue
        require(stat.S_ISREG(path.stat().st_mode), 'SDK contains a non-regular file')
        name = path.relative_to(root).as_posix()
        actual.add(name)
        total += path.stat().st_size
        require(len(actual) <= MAX_FILES and total <= MAX_BYTES, 'Installation storage bound')
        require(name in files, 'Unlisted SDK file: ' + name)
        require(file_hash(path, deadline, cancelled) == files[name], 'SDK file changed: ' + name)
    require(actual == files.keys(), 'SDK file roster incomplete')
    require(file_hash(manifest, deadline, cancelled) == identity, 'Manifest changed during validation')
    return identity, data


def seal_installation(host, qualification, host_receipt, manifest, timeout=60):
    """Seal an already qualified SDK without accepting a replacement source ID.

    qualification is installed_viewport.py's complete receipt; host_receipt is
    its host-evidence/session.json. Both remain outside the installed SDK.
    """
    require(type(timeout) in (int, float) and 0 < timeout <= 60, 'Seal deadline')
    deadline = time.monotonic() + timeout
    host, qualification, host_receipt, manifest = map(safe_path, (host, qualification, host_receipt, manifest))
    root = manifest.parent
    require(host.parent == root / 'bin', 'Host must belong to manifest SDK root')
    require(not qualification.is_relative_to(root) and not host_receipt.is_relative_to(root),
            'Qualification receipts must remain outside SDK')
    require(not manifest.exists(), 'Installation manifest already exists')
    require(qualification.stat().st_size <= 4 * 1024 * 1024 and host_receipt.stat().st_size <= 4 * 1024 * 1024,
            'Qualification receipt bound')
    evidence = json.loads(qualification.read_bytes(), object_pairs_hook=_object)
    native = json.loads(host_receipt.read_bytes(), object_pairs_hook=_object)
    require(evidence.get('passed') is True and native.get('schema') == 'luminumbra.viewport.session.v1'
            and native.get('status') == 'complete' and native.get('shutdown_complete') is True,
            'Successful installed host qualification required')
    require(evidence.get('session_sha256') == file_hash(host_receipt, deadline), 'Host receipt identity mismatch')
    for field in ('source_commit', 'source_dirty', 'source_input_sha256'):
        require(evidence.get(field) == native.get(field), 'Qualification source identity mismatch')
    files = evidence.get('inputs_before', {}).get('sdk')
    require(type(files) is dict and files == evidence.get('inputs_after', {}).get('sdk'),
            'Qualification SDK inputs changed or are absent')
    executable = host.relative_to(root).as_posix()
    require(files.get(executable) == native.get('executable_sha256'), 'Qualified executable identity mismatch')
    module = renderer_module(files)
    require(files[module] == native.get('module_sha256'), 'Qualified module identity mismatch')
    resources = native.get('resources', {})
    require(set(resources) == {Path(name).name for name in SHADERS}, 'Qualified shader roster incomplete')
    for name in SHADERS:
        require(files.get(name) == resources[Path(name).name], 'Qualified shader identity mismatch')
    value = {'schema': 'luminumbra.static_preview.installation.v1', 'source_commit': native['source_commit'],
             'source_dirty': native['source_dirty'], 'source_input_sha256': native['source_input_sha256'],
             'executable': executable, 'files': files, 'identity_receipt_sha256': file_hash(qualification, deadline)}
    # The fresh output is the only file excluded from the complete SDK roster.
    # Refusal removes only this newly owned output, never an existing manifest.
    owned = False
    try:
        with manifest.open('xb') as stream:
            owned = True
            stream.write(canonical(value))
        verify_installation(manifest, host, deadline)
    except BaseException:
        if owned:
            manifest.unlink(missing_ok=True)
        raise
    return value
