#!/usr/bin/env python3
"""Execution runner and aggregate for the render qualification matrix.

Cells run in matrix order. A ready cell is launched through
render_runner_process.run_process, its capture is bound to the cell, and then it is
judged by validate_render_capture.py with --enforce-floor; the checker's verdict must
reconcile with the capture. An unready cell is recorded as unevaluated and never
launched. The result is written to <out-dir>/qualification.json
(luminumbra.render_qualification.v1).

A --client path ending in .py runs with the current interpreter; the tests use this
to drive a fake client. Importing this module has no side effects.
"""
import argparse
import hashlib
import json
import math
import os
import re
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

import perf
import plan_qualification as planner
import qualification_matrix as matrix_lib
import render_budget
import render_runner_process as runner
from check_gl_debug_log import MAX_FILE_BYTES

SCHEMA = 'luminumbra.render_qualification.v1'
VERDICT_SCHEMA = 'luminumbra.render_verdict.v1'
VERDICT_KEYS = ('schema', 'workload_kind', 'profile', 'adapter', 'frames', 'floor', 'target')
FLOOR_KEYS = ('limit_ms', 'p99_ms', 'verdict')
STATES = ('pass', 'fail', 'invalid', 'process_failed', 'unevaluated')
DEFAULT_TIMEOUT_SECONDS = 900
CHECKER_TIMEOUT_SECONDS = 120
FLOOR_FAILURE_EXIT = 4
QUALIFICATION_NAME = 'qualification.json'
LOCK_NAME = '.qualification.lock'
BUILD_SCHEMA = 'luminumbra.performance_build.v1'
EXPANDED_REASON = 'requires far_coverage evidence from the far-world runtime workstream'
REFRESH_TEXT = re.compile(r'[0-9]{1,4}')
SETTINGS_NAME = 'settings'
SETTINGS_KEYS = ('HOME', 'USERPROFILE', 'APPDATA', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME')
RUNTIME_SENTINELS = ('res/shaders/g_buffer.vert', 'data/audio/music.bank.json',
                     'worlds/atlas/presets/default.json')
EXPECTED_FOV = 45.0  # The client default from SystemConfig.
# Geometry and settling checks are supplied by the matrix checker command.
FIXED_VIEW_CHECK_FLAGS = ('--require-camera-chunk',)
MAX_HOST_JSON_DEPTH = 64  # Object/array levels, checked before recursive JSON decoding.

# The canonical qualification universe. A run qualifies only against exactly these.
CANONICAL_RENDERER = 'NVIDIA GeForce RTX 5070 Ti'
CANONICAL_KINDS = {
    'forest-settled': 'fixed_view',
    'ground-settled': 'fixed_view',
    'high-altitude-settled': 'fixed_view',
    'cave-air-settled': 'fixed_view',
    'shoreline-settled': 'fixed_view',
    'mountain-vista-settled': 'fixed_view',
    'surface-flight': 'traversal',
    'cave-walk': 'traversal',
    'edited-world-tour': 'traversal',
}
CANONICAL_WORKLOADS = tuple(CANONICAL_KINDS)
CANONICAL_PROFILES = ('quality', 'performance')
CANONICAL_DISPLAYS = ('3840x1600', '3440x1440')
CANONICAL_WARMUP = 400
CANONICAL_FIXED_FRAMES = 240
CANONICAL_SURFACE_SCRIPT = 'tools/perf/fixtures/surface-flight.traversal'
# The traversal script each traversal workload must carry to be canonical. cave-walk and
# edited-world-tour are deliberately absent: they stay non-canonical (never qualify) until
# AC8 commits their scripts and binds them here.
BOUND_TRAVERSAL_SCRIPTS = {'surface-flight': CANONICAL_SURFACE_SCRIPT}
BOUND_TRAVERSAL_SHA256 = {
    'surface-flight': '029e54d693c993472d84d7a9a38b2c3bbf513f129a1922534906c0f91b479e4e',
}
TRAVERSAL_MAX_BYTES = 64 * 1024  # Traversal manifests are bounded to 64 KiB.


class UsageError(ValueError):
    """Operator input error; reported on stderr with exit code 2."""


def canonical_cell_ids():
    return frozenset(f'{workload}.{profile}.{display}'
                     for workload in CANONICAL_WORKLOADS
                     for profile in CANONICAL_PROFILES
                     for display in CANONICAL_DISPLAYS)


def sanitized_environment():
    """Copy of os.environ without LUMIN_ and LUMINUMBRA_ variables."""
    return {key: value for key, value in os.environ.items()
            if not key.startswith(('LUMIN_', 'LUMINUMBRA_'))}


def isolated_environment(out_dir, environment=None):
    """Client environment with user-settings locations redirected into out-dir/settings."""
    settings = Path(out_dir) / SETTINGS_NAME
    result = sanitized_environment() if environment is None else dict(environment)
    for key in SETTINGS_KEYS:
        result[key] = str(settings)
    return result


def reset_settings(out_dir):
    """Remove whatever an earlier run left in out-dir/settings, then recreate it empty."""
    settings = Path(out_dir) / SETTINGS_NAME
    if settings.is_dir():
        shutil.rmtree(settings, ignore_errors=False)
    elif settings.exists():
        settings.unlink()
    settings.mkdir(parents=True)


def set_aside(path):
    """Rename an existing file to the smallest unused <name>.prev-<n>; never delete it."""
    if not path.exists():
        return
    number = 1
    while path.with_name(f'{path.name}.prev-{number}').exists():
        number += 1
    path.rename(path.with_name(f'{path.name}.prev-{number}'))


def read_json_object(path):
    """Parsed JSON object from path, or None when missing, unreadable or not an object."""
    try:
        data = json.loads(Path(path).read_text(encoding='utf-8-sig'))
    except (OSError, ValueError, UnicodeError):
        return None
    return data if isinstance(data, dict) else None


def read_host_bytes(path, *, missing_ok=False):
    """Read only regular, non-symlink host files within the shared 16 MiB cap."""
    try:
        path = Path(path)
        try:
            info = path.lstat()
        except FileNotFoundError:
            if missing_ok:
                return None
            raise
        if not stat.S_ISREG(info.st_mode):
            raise UsageError(f'host evidence must be a regular file, not a symlink: {path}')
        if info.st_size > MAX_FILE_BYTES:
            raise UsageError(f'host evidence exceeds {MAX_FILE_BYTES} byte cap: {path}')
        # Do not follow a replacement symlink or block on a replacement FIFO.
        flags = (os.O_RDONLY | getattr(os, 'O_BINARY', 0)
                 | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0))
        with os.fdopen(os.open(path, flags), 'rb') as source:
            opened = os.fstat(source.fileno())
            if not stat.S_ISREG(opened.st_mode):
                raise UsageError(f'host evidence must be a regular file: {path}')
            if (opened.st_dev, opened.st_ino) != (info.st_dev, info.st_ino):
                raise UsageError(f'host evidence changed while opening: {path}')
            if opened.st_size > MAX_FILE_BYTES:
                raise UsageError(f'host evidence exceeds {MAX_FILE_BYTES} byte cap: {path}')
            payload = source.read(MAX_FILE_BYTES + 1)
        if len(payload) > MAX_FILE_BYTES:
            raise UsageError(f'host evidence exceeds {MAX_FILE_BYTES} byte cap: {path}')
        return payload
    except UsageError:
        raise
    except (OSError, ValueError) as error:
        raise UsageError(f'cannot read host evidence {path}: {error}') from error


def check_host_json_depth(text):
    """Bound containers before decoding; brackets inside JSON strings do not count."""
    depth = 0
    in_string = escaped = False
    for char in text:
        if in_string:
            if escaped:
                escaped = False
            elif char == '\\':
                escaped = True
            elif char == '"':
                in_string = False
        elif char == '"':
            in_string = True
        elif char in '[{':
            depth += 1
            if depth > MAX_HOST_JSON_DEPTH:
                raise UsageError(f'JSON depth exceeds {MAX_HOST_JSON_DEPTH} container levels')
        elif char in ']}':
            depth -= 1


def load_host_record(path):
    """Validate staged wrapper evidence before any existing run is modified."""
    if path is None:
        return None
    try:
        payload = read_host_bytes(path).decode('utf-8-sig')
        check_host_json_depth(payload)
        document = json.loads(payload)
    except (ValueError, UnicodeError, RecursionError) as error:
        raise UsageError(f'--host-record: {error}') from error

    def text(value):
        return isinstance(value, str) and bool(value.strip()) and '\x00' not in value

    if (not isinstance(document, dict)
            or any(not text(document.get(key))
                   for key in ('os_caption', 'os_version', 'powershell'))
            or not isinstance(document.get('gpus'), list) or not document['gpus']
            or any(not isinstance(gpu, dict)
                   or any(not text(gpu.get(key)) for key in ('Name', 'DriverVersion'))
                   for gpu in document['gpus'])):
        raise UsageError('--host-record: expected OS, PowerShell and GPU driver identity')
    try:
        record = (json.dumps(document, ensure_ascii=False, separators=(',', ':'),
                             allow_nan=False) + '\n').encode('utf-8')
    except (ValueError, UnicodeError, RecursionError) as error:
        raise UsageError(f'--host-record: {error}') from error
    if len(record) > MAX_FILE_BYTES:
        raise UsageError(f'--host-record: serialized host evidence exceeds '
                         f'{MAX_FILE_BYTES} byte cap')
    return record


def publish_evidence(path, payload, host_record=None, *, previous_host):
    """Atomically replace each record under the qualification lock.

    Retire the host for hostless runs. Restore the validated previous snapshot if
    aggregate publication fails, so host and aggregate provenance stay together.
    """
    host_path = path.parent / 'host.json'
    if host_record is None:
        host_path.unlink(missing_ok=True)
    else:
        runner.atomic_write(host_path, host_record)
    try:
        runner.atomic_write(path, payload)
    except OSError:
        if previous_host is None:
            host_path.unlink(missing_ok=True)
        else:
            runner.atomic_write(host_path, previous_host)
        raise


def finite(value):
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value))


def adapter_triple(data):
    adapter = data.get('adapter') if isinstance(data, dict) else None
    if not isinstance(adapter, dict):
        return None
    return {key: adapter.get(key) for key in ('vendor', 'renderer', 'version')}


def finish(cell, state, reason, artifact, verdict=None, *, checker_canonical,
           out_dir, matrix_canonical, client_evidence, traversal_evidence=None):
    """Cell result; the artifact path and adapter are reported only when the file exists."""
    present = Path(artifact).is_file()
    return {'id': cell['id'], 'workload': cell['workload'], 'kind': cell['kind'],
                'warmup_frames': cell['warmup_frames'], 'measured_frames': cell['measured_frames'],
                'script': cell['script'], 'state': state, 'reason': reason,
                'adapter': adapter_triple(read_json_object(artifact)) if present else None,
                'artifact': Path(artifact).relative_to(out_dir).as_posix() if present else None,
                'verdict': verdict, 'checker_canonical': checker_canonical,
                'matrix_canonical': matrix_canonical, 'client': client_evidence,
                'traversal': traversal_evidence}


def binding_mismatch(cell, data):
    """Name the first capture field that disagrees with the cell, or None when all match."""
    profile = data.get('profile')
    if not isinstance(profile, dict) or profile.get('name') != cell['profile']:
        return 'profile'
    if data.get('width') != cell['width']:
        return 'width'
    if data.get('height') != cell['height']:
        return 'height'
    if data.get('warmup_frames') != cell['warmup_frames']:
        return 'warmup_frames'
    if cell['kind'] == 'fixed_view' and data.get('frames') != cell['measured_frames']:
        return 'frames'
    return None


def verdict_reason(verdict, data, cell):
    """None when a checker-passed verdict reconciles with the capture and cell, else the reason."""
    # --verdict-out writes render_budget.evaluate(data), a full v1 envelope, despite
    # the checker's local variable being named 'floor'. Do not accept a bare floor.
    if not isinstance(verdict, dict) or any(key not in verdict for key in VERDICT_KEYS):
        return 'verdict missing/malformed'
    floor = verdict['floor']
    if (verdict['schema'] != VERDICT_SCHEMA or not isinstance(floor, dict)
            or any(key not in floor for key in FLOOR_KEYS)
            or not isinstance(verdict['target'], dict)):
        return 'verdict missing/malformed'
    if (floor['verdict'] not in ('pass', 'fail') or floor['limit_ms'] != render_budget.FLOOR_MS
            or not finite(floor['p99_ms'])):
        return 'verdict missing/malformed'
    if floor['verdict'] == 'fail':
        return 'verdict-disagrees-with-checker'
    if floor['p99_ms'] > floor['limit_ms']:
        return 'verdict-does-not-match-capture: p99_ms'
    distribution = data.get('distribution')
    block = distribution.get('frame_wall_ms') if isinstance(distribution, dict) else None
    capture_p99 = block.get('p99') if isinstance(block, dict) else None
    if not finite(capture_p99):
        return 'capture-lacks-frame-wall-p99'
    profile = data.get('profile')
    name = profile.get('name') if isinstance(profile, dict) else None
    if verdict['profile'] != name:
        return 'verdict-does-not-match-capture: profile'
    # The checker's workload_kind is the capture's workload.kind, which uses the same
    # 'fixed_view' / 'traversal' vocabulary as the matrix cell kind, so no mapping is needed.
    capture_workload = data.get('workload')
    capture_kind = capture_workload.get('kind') if isinstance(capture_workload, dict) else None
    if verdict['workload_kind'] != cell['kind'] or capture_kind not in (None, cell['kind']):
        return 'verdict-does-not-match-capture: workload_kind'
    if verdict['adapter'] != adapter_triple(data):
        return 'verdict-does-not-match-capture: adapter'
    if verdict['frames'] != data.get('frames'):
        return 'verdict-does-not-match-capture: frames'
    if floor['p99_ms'] != capture_p99:
        return 'verdict-does-not-match-capture: p99_ms'
    return None


def verdict_passes(verdict):
    return (isinstance(verdict, dict) and verdict.get('schema') == VERDICT_SCHEMA
            and isinstance(verdict.get('floor'), dict)
            and verdict['floor'].get('verdict') == 'pass')


def valid_adapter(adapter):
    return (isinstance(adapter, dict)
            and all(isinstance(adapter.get(key), str) and adapter.get(key)
                    for key in ('vendor', 'renderer', 'version')))


def canonical_record(result):
    """True when the cell is bound to its id and carries exactly the canonical semantics."""
    if result.get('matrix_canonical') is not True:
        return False
    cell_id = result['id']
    parts = cell_id.split('.') if isinstance(cell_id, str) else []
    if len(parts) != 3 or cell_id not in canonical_cell_ids():
        return False
    workload, profile, display = parts
    if result['workload'] != workload:
        return False
    if result.get('profile', profile) != profile or result.get('display', display) != display:
        return False
    if CANONICAL_KINDS.get(result['workload']) != result['kind']:
        return False
    if result['warmup_frames'] != CANONICAL_WARMUP:
        return False
    if result['kind'] == 'fixed_view':
        return result['measured_frames'] == CANONICAL_FIXED_FRAMES
    traversal = result.get('traversal') or {}
    expected = BOUND_TRAVERSAL_SHA256.get(result['workload'])
    return (result['script'] is not None
            and BOUND_TRAVERSAL_SCRIPTS.get(result['workload']) == result['script']
            and expected is not None and traversal.get('sha256') == expected
            and traversal.get('verified') is True)


def cell_qualifies(result):
    """A cell counts toward qualification only with a complete, canonical, passing record
    that was checked by the canonical checker."""
    client = result.get('client') or {}
    return (client.get('kind') == 'native' and client.get('native_build') is True
            and client.get('sha256_verified') is True
            and isinstance(client.get('sha256'), str)
            and re.fullmatch(r'[0-9a-f]{64}', client['sha256']) is not None
            and client['sha256'] == client.get('build_manifest_client_sha256')
            and result['state'] == 'pass' and valid_adapter(result['adapter'])
            and result['adapter']['renderer'] == CANONICAL_RENDERER
            and result['artifact'] is not None and verdict_passes(result['verdict'])
            and result['checker_canonical'] is True and canonical_record(result))


def matrix_identity(matrix, path=planner.DEFAULT_MATRIX):
    """Hash all matrix content, including poses/provenance, against the repo matrix."""
    def digest(value):
        payload = json.dumps(value, sort_keys=True, separators=(',', ':'),
                             allow_nan=False).encode('utf-8')
        return hashlib.sha256(payload).hexdigest()

    sha256 = digest(matrix)
    canonical_sha256 = digest(matrix_lib.load_matrix(planner.DEFAULT_MATRIX))
    canonical = (runner.resolve_path(path) == runner.resolve_path(planner.DEFAULT_MATRIX)
                 and sha256 == canonical_sha256)
    return {'sha256': sha256, 'canonical_sha256': canonical_sha256, 'canonical': canonical}


def load_build_manifest(path):
    """Existing performance-build schema, extended with the built client's SHA-256."""
    if path is None:
        return None
    try:
        before = runner.file_sha256(path)
        document = read_json_object(path)
        if document is None or document.get('schema') != BUILD_SCHEMA:
            raise ValueError('schema must be ' + BUILD_SCHEMA)
        # Reuse the repo's compiler/configuration validation for this same schema.
        perf.load_build_manifest(Path(path))
        system = document.get('system')
        if (not isinstance(system, dict)
                or any(not isinstance(system.get(key), str) or not system[key]
                       for key in ('name', 'processor'))):
            raise ValueError('system identity is incomplete')
        client = document.get('client')
        if (not isinstance(client, dict) or set(client) != {'sha256'}
                or not isinstance(client['sha256'], str)
                or re.fullmatch(r'[0-9a-f]{64}', client['sha256']) is None):
            raise ValueError('client must contain a lowercase SHA-256 in client.sha256')
        if runner.file_sha256(path) != before:
            raise ValueError('manifest changed while being read')
        return {'sha256': before, 'client_sha256': client['sha256']}
    except (OSError, ValueError, UnicodeError) as error:
        raise UsageError(f'--build-manifest: {error}') from error


def native_executable(path):
    """Require an executable image, excluding formats dispatched through interpreters."""
    path = Path(path)
    script_suffixes = {'.bat', '.cmd', '.com', '.ps1', '.py', '.pyw', '.sh', '.bash',
                       '.csh', '.zsh', '.fish', '.pl', '.rb', '.js', '.vbs', '.wsf'}
    if (path.suffix.lower() in script_suffixes
            or runner.resolve_path(path).suffix.lower() in script_suffixes
            or (os.name == 'nt' and path.suffix.lower() != '.exe')
            or not os.access(path, os.X_OK)):
        return False
    with path.open('rb') as source:
        size = os.fstat(source.fileno()).st_size

        def read(offset, count):
            if offset < 0 or offset + count > size:
                return b''
            source.seek(offset)
            return source.read(count)

        header = read(0, 64)
        if len(header) != 64:
            return False
        if header[:2] == b'MZ':
            pe_offset = struct.unpack_from('<I', header, 60)[0]
            coff = read(pe_offset, 24)
            if pe_offset < 64 or len(coff) != 24 or coff[:4] != b'PE\0\0':
                return False
            machine, sections = struct.unpack_from('<HH', coff, 4)
            optional_size, flags = struct.unpack_from('<HH', coff, 20)
            optional = read(pe_offset + 24, optional_size)
            if (machine not in (0x14c, 0x8664, 0xaa64) or not 1 <= sections <= 96
                    or not flags & 0x0002 or flags & 0x2000
                    or len(optional) < 96):
                return False
            magic = struct.unpack_from('<H', optional)[0]
            minimum = {0x10b: 96, 0x20b: 112}.get(magic)
            if minimum is None or len(optional) < minimum:
                return False
            directories = struct.unpack_from('<I', optional, minimum - 4)[0]
            if directories > 16 or minimum + directories * 8 > len(optional):
                return False
            entry, = struct.unpack_from('<I', optional, 16)
            image_size, headers_size = struct.unpack_from('<II', optional, 56)
            table_offset = pe_offset + 24 + optional_size
            table = read(table_offset, sections * 40)
            if (len(table) != sections * 40 or not 0 < entry < image_size
                    or not table_offset + len(table) <= headers_size <= size):
                return False
            executable_entry = False
            for offset in range(0, len(table), 40):
                virtual_size, address, raw_size, raw_offset = struct.unpack_from(
                    '<IIII', table, offset + 8)
                section_flags, = struct.unpack_from('<I', table, offset + 36)
                if raw_size and (raw_offset < headers_size or raw_offset + raw_size > size):
                    return False
                executable_entry |= (bool(section_flags & 0x20000000)
                                     and address <= entry < address + max(virtual_size, raw_size))
            return executable_entry
        if header[:4] == b'\x7fELF':
            if header[4] not in (1, 2) or header[5] not in (1, 2) or header[6] != 1:
                return False
            endian = '<' if header[5] == 1 else '>'
            kind, machine, version = struct.unpack_from(endian + 'HHI', header, 16)
            wide = header[4] == 2
            entry, phoff = struct.unpack_from(endian + ('QQ' if wide else 'II'), header, 24)
            ehsize, phsize, phnum = struct.unpack_from(endian + 'HHH', header, 52 if wide else 40)
            return (kind in (2, 3) and machine != 0 and version == 1 and entry != 0
                    and ehsize == (64 if wide else 52) and phsize == (56 if wide else 32)
                    and phnum > 0 and ehsize <= phoff and phoff + phsize * phnum <= size)

        def macho(offset, length):
            header = read(offset, 32)
            formats = {b'\xfe\xed\xfa\xce': ('>', 28), b'\xce\xfa\xed\xfe': ('<', 28),
                       b'\xfe\xed\xfa\xcf': ('>', 32), b'\xcf\xfa\xed\xfe': ('<', 32)}
            if len(header) != 32 or header[:4] not in formats:
                return False
            endian, header_size = formats[header[:4]]
            filetype, commands, command_size = struct.unpack_from(endian + 'III', header, 12)
            return (filetype == 2 and commands > 0 and command_size >= commands * 8
                    and header_size + command_size <= length)

        if header[:4] in (b'\xca\xfe\xba\xbe', b'\xbe\xba\xfe\xca'):
            endian = '>' if header[:4] == b'\xca\xfe\xba\xbe' else '<'
            count, = struct.unpack_from(endian + 'I', header, 4)
            table = read(8, count * 20) if 1 <= count <= 64 else b''
            if not table:
                return False
            for index in range(count):
                offset, length = struct.unpack_from(endian + 'II', table, index * 20 + 8)
                if offset < 8 + len(table) or offset + length > size or not macho(offset, length):
                    return False
            return True
        return macho(0, size)


def run_client_process(command, *, client, build_manifest, systems_config,
                       runtime_root, save_root, **kwargs):
    """Bind client-code and systems configuration identities to the process receipt."""
    script = str(client).endswith('.py')
    path = (runner.resolve_path(client) if script else
            runner.resolve_executable(command[0], kwargs['cwd'], kwargs['environment']))
    sha256 = runner.file_sha256(path)
    config_sha256 = runner.file_sha256(systems_config)
    native = not script and native_executable(path)
    identity = {'sha256': sha256, 'sha256_verified': False, 'native_build': False,
                    'kind': 'native' if native else 'script', 'reason': 'client_not_native_build',
                    'build_manifest_client_sha256': None if build_manifest is None else
                    build_manifest['client_sha256'],
                    'build_manifest_sha256': None if build_manifest is None else
                    build_manifest['sha256']}
    errors = []

    def finalize_receipt():
        receipt = read_json_object(kwargs['receipt_path'])
        if receipt is not None:
            try:
                verified = runner.file_sha256(path) == sha256
            except OSError as error:
                verified = False
                errors.append(f'client identity read: {error}')
            if not script:
                verified = verified and receipt.get('binary_sha256') == sha256
            identity['sha256_verified'] = verified
            if not verified:
                errors.append('client SHA-256 changed or disagrees with process receipt')
            if native:
                identity['reason'] = ('build_manifest_missing' if build_manifest is None
                                      else 'client_build_hash_mismatch')
                if (verified and build_manifest is not None
                        and build_manifest['client_sha256'] == sha256):
                    identity.update(native_build=True, reason=None)
            try:
                config_verified = runner.file_sha256(systems_config) == config_sha256
            except OSError as error:
                config_verified = False
                errors.append(f'systems configuration read: {error}')
            if not config_verified:
                errors.append('systems configuration SHA-256 changed during execution')
            receipt.update(client_path=str(path), client=identity,
                           runtime_root=str(runtime_root), save_root=str(save_root),
                           systems_config={'path': str(systems_config), 'sha256': config_sha256,
                                           'sha256_verified': config_verified})
            try:
                capture_path = receipt.get('capture_path')
                if capture_path is not None:
                    receipt['capture_path'] = Path(capture_path).relative_to(
                        Path(kwargs['receipt_path']).parent.parent).as_posix()
                receipt['evidence_errors'] = list(dict.fromkeys(
                    receipt.get('evidence_errors', []) + errors))
                runner.atomic_write(kwargs['receipt_path'],
                                    (json.dumps(receipt, indent=2) + '\n').encode('utf-8'))
            except OSError as error:
                errors.append(f'client identity evidence: {error}')
        else:
            errors.append('client identity: process receipt missing/malformed')
    completed, process_errors = runner.run_process(
        command, finalize_receipt=finalize_receipt, **kwargs)
    return completed, process_errors + errors, identity


def check_command(cell, *, matrix, artifact, verdict_path, root, python, checker):
    """Checker argv: the matrix checker command with fixed-view report checks."""
    command = matrix_lib.checker_command(
        cell, artifact=artifact, verdict=verdict_path,
        qualified_renderer=matrix['qualified_renderer'],
        root=root, python=python)
    command[1] = str(checker)
    if cell['kind'] == 'fixed_view':
        command += list(FIXED_VIEW_CHECK_FLAGS) + ['--fov', str(EXPECTED_FOV)]
    return command


def capture_ownership(receipt, retained, capture, out_dir):
    """Return a refusal reason unless the receipt owns the retained capture bytes."""
    if receipt is None or receipt.get('schema') != runner.RECEIPT_SCHEMA:
        return 'capture evidence: process receipt missing/malformed'
    if receipt.get('capture_status') != 'retained':
        return 'capture evidence: capture not retained'
    if (receipt.get('outcome') != 'returned' or receipt.get('return_code') != 0
            or receipt.get('binary_identity_status') != 'verified'
            or receipt.get('evidence_errors')):
        return 'capture evidence: process receipt unsuccessful'
    if receipt.get('capture_path') != retained.relative_to(out_dir).as_posix():
        return 'capture evidence: retained path mismatch'
    command = receipt.get('command')
    if not isinstance(command, list) or command.count('--render-benchmark') != 1:
        return 'capture evidence: capture command missing/malformed'
    index = command.index('--render-benchmark') + 1
    if index >= len(command) or command[index] != str(capture):
        return 'capture evidence: capture command path mismatch'
    try:
        digest = runner.file_sha256(retained)
        if digest != receipt.get('capture_sha256') or digest != runner.file_sha256(capture):
            return 'capture evidence: retained hash mismatch'
    except OSError as error:
        return f'capture evidence: capture read failed: {error}'
    return None


def runtime_options(systems_config, runtime_root, save_root):
    """The configuration is a declared override; the roots select file locations."""
    return ['--systems-config', str(systems_config), '--runtime-root', str(runtime_root),
            '--save-root', str(save_root)]


def run_cell(cell, matrix, *, client, out_dir, root, python, checker, timeout_seconds,
             environment, checker_canonical, matrix_canonical, build_manifest, runtime_root):
    state, reason = matrix_lib.readiness(cell, root)
    cells_dir = out_dir / 'cells'
    cell_id = cell['id']
    artifact = cells_dir / f'{cell_id}.json'
    client_evidence = None
    traversal_evidence = None

    def record(state, reason, artifact, verdict=None):
        return finish(cell, state, reason, artifact, verdict,
                      checker_canonical=checker_canonical, out_dir=out_dir,
                      matrix_canonical=matrix_canonical, client_evidence=client_evidence,
                      traversal_evidence=traversal_evidence)

    verdict_path = cells_dir / f'{cell_id}.verdict.json'
    for previous in (artifact, verdict_path, cells_dir / f'{cell_id}.artifact.json',
                     cells_dir / f'{cell_id}.process.json'):
        set_aside(previous)
    if state != 'ready':
        return record('unevaluated', reason, artifact)
    work = out_dir / 'work' / cell_id
    work.mkdir(parents=True, exist_ok=True)
    save_root = work / 'storage'
    save_root.mkdir(parents=True, exist_ok=True)
    cells_dir.mkdir(parents=True, exist_ok=True)
    retained = cells_dir / f'{cell_id}.artifact.json'
    receipt_path = cells_dir / f'{cell_id}.process.json'
    try:
        with tempfile.TemporaryDirectory(prefix='.capture-', dir=work) as staging:
            capture = Path(staging) / 'capture.json'
            command = matrix_lib.build_command(cell, client=client, artifact=capture, root=root)
            systems_config = runner.resolve_path(runtime_root / 'data/common/systems.json')
            command += runtime_options(systems_config, runtime_root, save_root)
            declaration = work / 'render-overrides.json'
            runner.atomic_write(declaration, json.dumps(
                {'cli:--systems-config': str(systems_config)}).encode('utf-8'))
            command += ['--declare-render-overrides', str(declaration)]
            traversal_path = None
            if cell['kind'] == 'traversal':
                with (Path(root) / cell['script']).open('rb') as source:
                    payload = source.read(TRAVERSAL_MAX_BYTES + 1)
                if len(payload) > TRAVERSAL_MAX_BYTES:
                    return record('invalid', 'traversal exceeds 64 KiB', artifact)
                traversal_evidence = {'sha256': hashlib.sha256(payload).hexdigest(),
                                      'verified': False}
                if traversal_evidence['sha256'] != cell['script_sha256']:
                    return record('invalid', 'traversal SHA-256 mismatch', artifact)
                traversal_path = work / 'workload.traversal'
                runner.atomic_write(traversal_path, payload)
                command[command.index('--traversal') + 1] = str(traversal_path)
            if str(client).endswith('.py'):
                command = [sys.executable, str(client)] + command[1:]
            completed, evidence, client_evidence = run_client_process(
                command, client=client, build_manifest=build_manifest,
                systems_config=systems_config, runtime_root=runtime_root,
                save_root=save_root, cwd=work,
                environment=environment, log=cells_dir / f'{cell_id}.log',
                artifact=capture, retained=retained, receipt_path=receipt_path,
                timeout_seconds=timeout_seconds)
            if traversal_path is not None:
                traversal_evidence['verified'] = (
                    runner.file_sha256(traversal_path) == traversal_evidence['sha256'])
                if not traversal_evidence['verified']:
                    return record('invalid', 'traversal changed during execution', artifact)
            receipt = read_json_object(receipt_path)
            ownership = capture_ownership(receipt, retained, capture, out_dir)
            if ownership is None:
                capture_sha256 = receipt['capture_sha256']
                payload = retained.read_bytes()
                if hashlib.sha256(payload).hexdigest() != capture_sha256:
                    ownership = 'capture evidence: retained hash mismatch'
                else:
                    runner.atomic_write(artifact, payload)
                if ownership is None and runner.file_sha256(artifact) != capture_sha256:
                    ownership = 'capture evidence: published hash mismatch'
    except subprocess.TimeoutExpired:
        receipt = read_json_object(cells_dir / f'{cell_id}.process.json')
        client_evidence = None if receipt is None else receipt.get('client')
        return record('process_failed', 'timed_out', artifact)
    except OSError as error:
        receipt = read_json_object(cells_dir / f'{cell_id}.process.json')
        client_evidence = None if receipt is None else receipt.get('client')
        return record('process_failed', f'spawn-failed: {error}', artifact)
    if completed.returncode != 0:
        return record('process_failed', f'exit {completed.returncode}', artifact)
    if evidence:
        return record('process_failed', 'evidence: ' + '; '.join(evidence), artifact)
    if not artifact.is_file():
        return record('process_failed', ownership or 'no-artifact', artifact)
    if ownership is not None:
        return record('process_failed', ownership, artifact)
    data = read_json_object(artifact)
    field = None if data is None else binding_mismatch(cell, data)
    if field is not None:
        return record('invalid', f'capture-does-not-match-cell: {field}', artifact)
    checked_cell = cell if traversal_path is None else dict(cell, script=str(traversal_path))
    check = check_command(checked_cell, matrix=matrix, artifact=artifact, verdict_path=verdict_path,
                          root=root, python=python, checker=checker)
    try:
        with (cells_dir / f'{cell_id}.check.log').open('wb') as log:
            checked = subprocess.run(check, cwd=work, env=environment, stdout=log,
                                     stderr=subprocess.STDOUT,
                                     timeout=CHECKER_TIMEOUT_SECONDS, check=False)
    except subprocess.TimeoutExpired:
        return record('invalid',
                      f'checker timed out after {CHECKER_TIMEOUT_SECONDS} s', artifact)
    except OSError as error:
        return record('invalid', f'checker spawn-failed: {error}', artifact)
    if traversal_path is not None:
        try:
            traversal_evidence['verified'] = (
                runner.file_sha256(traversal_path) == traversal_evidence['sha256'])
        except OSError:
            traversal_evidence['verified'] = False
        if not traversal_evidence['verified']:
            return record('invalid', 'traversal changed during checking', artifact)
    try:
        unchanged = (runner.file_sha256(artifact) == capture_sha256
                     and runner.file_sha256(retained) == capture_sha256)
    except OSError:
        unchanged = False
    if not unchanged:
        return record('invalid', 'capture evidence: checked hash mismatch', artifact)
    verdict = read_json_object(verdict_path)
    if checked.returncode == 0:
        reason = verdict_reason(verdict, data or {}, cell)
        if reason is not None:
            return record('invalid', reason, artifact, verdict)
        return record('pass', None, artifact, verdict)
    if checked.returncode == FLOOR_FAILURE_EXIT:
        return record('fail', 'floor', artifact, verdict)
    return record('invalid', f'checker exit {checked.returncode}', artifact, verdict)


def run_cells(matrix, *, client, out_dir, root=planner.ROOT, python=sys.executable,
              only=None, timeout_seconds=DEFAULT_TIMEOUT_SECONDS, checker=None,
              environment=None, matrix_canonical=None, build_manifest=None, runtime_root=None):
    """Run every selected cell in matrix order; return one result dict per cell."""
    out_dir = runner.resolve_path(out_dir)
    client = resolve_client(str(client))
    if not Path(client).is_absolute():
        client = str(runner.resolve_executable(client, Path.cwd(), environment))
    runtime_root = resolve_runtime_root(client, runtime_root)
    if matrix_canonical is None:
        matrix_canonical = matrix_identity(matrix)['canonical']
    if checker is None:
        checker = Path(root) / matrix_lib.VALIDATOR
    # A replacement checker may run, but its results never count toward qualification.
    checker_canonical = (runner.resolve_path(checker)
                        == runner.resolve_path(Path(root) / matrix_lib.VALIDATOR))
    reset_settings(out_dir)
    environment = isolated_environment(out_dir, environment)
    return [run_cell(cell, matrix, client=client, out_dir=out_dir, root=root, python=python,
                     checker=checker, timeout_seconds=timeout_seconds,
                     environment=environment, checker_canonical=checker_canonical,
                     matrix_canonical=matrix_canonical, build_manifest=build_manifest,
                     runtime_root=runtime_root)
            for cell in planner.select(matrix_lib.cells(matrix), only)]


def display_entries(matrix, declared_refresh):
    """Displays with refresh_hz and refresh_source; a matrix value always wins."""
    entries = []
    for display in matrix['displays']:
        if display['refresh_hz'] is not None:
            hz, source = display['refresh_hz'], 'matrix'
        elif display['id'] in declared_refresh:
            hz, source = declared_refresh[display['id']], 'operator'
        else:
            hz, source = None, 'undeclared'
        entries.append({'id': display['id'], 'refresh_hz': hz, 'refresh_source': source})
    return entries


def aggregate(matrix, results, *, partial, declared_refresh, matrix_provenance=None):
    """Build the luminumbra.render_qualification.v1 document from cell results."""
    declared = dict(declared_refresh)
    provenance = matrix_identity(matrix) if matrix_provenance is None else matrix_provenance
    displays = display_entries(matrix, declared)
    summary = {state: 0 for state in STATES}
    for result in results:
        summary[result['state']] += 1
    # Identity is decided over every cell, so a missing adapter breaks it.
    adapters = [result['adapter'] for result in results]
    adapter_identical = (bool(adapters) and adapters[0] is not None
                         and all(item == adapters[0] for item in adapters))
    reported = [item for item in adapters if item is not None]
    qualified = matrix['qualified_renderer']
    adapter_matches = all(item['renderer'] == qualified for item in reported)
    harness_complete = (not partial and summary['process_failed'] == 0
                        and summary['invalid'] == 0)
    ids = [result['id'] for result in results]
    canonical = canonical_cell_ids()
    complete_universe = (not partial and len(ids) == len(canonical)
                         and set(ids) == canonical)
    undeclared = any(item['refresh_source'] == 'undeclared' for item in displays)
    render_floor = (provenance['canonical'] and complete_universe
                    and qualified == CANONICAL_RENDERER
                    and all(cell_qualifies(result) for result in results)
                    and adapter_identical and adapter_matches and not undeclared)
    reasons = []
    if not provenance['canonical'] or any(not result.get('matrix_canonical')
                                          for result in results):
        reasons.append('non_canonical_matrix')
    for result in results:
        client = result.get('client')
        if client is not None and client.get('reason') and client['reason'] not in reasons:
            reasons.append(client['reason'])
    if not render_floor and not reasons:
        reasons.append('qualification_evidence_incomplete')
    return {
        'schema': SCHEMA, 'matrix_schema': matrix['schema'], 'partial': partial,
        'matrix': provenance,
        'qualified_renderer': qualified, 'declared_refresh': declared, 'displays': displays,
        'cells': [{'id': result['id'], 'workload': result['workload'], 'kind': result['kind'],
                    'warmup_frames': result['warmup_frames'],
                    'measured_frames': result['measured_frames'], 'script': result['script'],
                    'state': result['state'], 'reason': result['reason'],
                    'adapter': result['adapter'], 'artifact': result['artifact'],
                    'verdict': result['verdict'], 'checker_canonical': result['checker_canonical'],
                    'matrix_canonical': result.get('matrix_canonical', False),
                    'client': result.get('client'), 'traversal': result.get('traversal')}
               for result in results],
        'summary': summary, 'adapter_identical': adapter_identical,
        'adapter_matches_qualified': adapter_matches, 'harness_complete': harness_complete,
        'render_floor_qualified': render_floor, 'expanded_qualified': False,
        'render_floor_qualified_reasons': reasons,
        'qualification_status': 'QUALIFIED' if render_floor else 'NEEDS-NATIVE-RUN',
        'expanded_qualified_reason': EXPANDED_REASON}


def parse_declarations(values, matrix):
    """Parse repeated <display-id>=<hz>; refuse unknown ids, bad values and conflicts."""
    known = {display['id']: display for display in matrix['displays']}
    declared = {}
    for value in values:
        display_id, separator, hz_text = value.partition('=')
        if not separator or display_id not in known:
            raise UsageError(f'--declare-refresh {value!r}: expected <display-id>=<hz> '
                             'naming a matrix display')
        if display_id in declared:
            raise UsageError(f'--declare-refresh {display_id!r} given more than once')
        if known[display_id]['refresh_hz'] is not None:
            raise UsageError(f'--declare-refresh {display_id!r}: the matrix already '
                             'records its refresh')
        if REFRESH_TEXT.fullmatch(hz_text) is None or not 1 <= int(hz_text) <= 1000:
            raise UsageError(f'--declare-refresh {value!r}: hz must be an integer 1..1000')
        declared[display_id] = int(hz_text)
    return declared


def exit_code(summary):
    if summary['fail'] or summary['invalid'] or summary['process_failed']:
        return 1
    if summary['unevaluated']:
        return 3
    return 0


def resolve_client(text):
    """Absolute path when the client is a path that exists from the caller's cwd."""
    if Path(text).exists():
        return str(runner.resolve_path(text))
    return text


def resolve_runtime_root(client, explicit=None):
    """Use only the explicit layout or ancestors of the resolved client executable."""
    if explicit is not None:
        explicit = Path(explicit)
        if not explicit.is_absolute():
            raise UsageError('--runtime-root requires an absolute directory')
        candidates = (runner.resolve_path(explicit),)
    else:
        executable = runner.resolve_path(client)
        candidates = executable.parents
    for candidate in candidates:
        if all((candidate / name).is_file() for name in RUNTIME_SENTINELS):
            return candidate
    raise UsageError('runtime root missing shader, audio or default-preset files; '
                     'supply --runtime-root with an absolute runtime directory')


def resolve_checker(text):
    """Absolute path when the checker script exists from the caller's cwd."""
    return str(runner.resolve_path(text)) if Path(text).exists() else text


def run_dry(matrix, args, out_dir):
    client = resolve_client(str(args.client))
    if not Path(client).is_absolute():
        client = str(runner.resolve_executable(client, Path.cwd()))
    runtime_root = resolve_runtime_root(client, args.runtime_root)
    plan = planner.build_plan(matrix, client=client, out_dir=out_dir, root=planner.ROOT,
                              python=sys.executable, only=args.only)
    for cell in plan['cells']:
        if cell['command'] is not None:
            save_root = out_dir / 'work' / cell['id'] / 'storage'
            systems_config = runner.resolve_path(runtime_root / 'data/common/systems.json')
            cell['command'] += runtime_options(systems_config, runtime_root, save_root)
            cell['command'] += ['--declare-render-overrides',
                                str(save_root.parent / 'render-overrides.json')]
    path = out_dir / planner.PLAN_NAME
    path.write_text(json.dumps(plan, indent=2) + '\n', encoding='utf-8')
    print(f"qualification plan: {plan['cell_count']} cells, "
          f"ready {plan['summary']['ready']}, "
          f"unevaluated {plan['summary']['unevaluated']} -> {path}")
    return 0


def fail_closed(path, error, host_record=None, *, previous_host):
    """Replace the aggregate with an unqualified error record; exit 1."""
    message = str(error) or type(error).__name__
    payload = {'schema': SCHEMA, 'render_floor_qualified': False, 'harness_complete': False,
                   'expanded_qualified': False, 'error': message,
                   'qualification_status': 'NEEDS-NATIVE-RUN',
                   'render_floor_qualified_reasons': ['qualification_run_failed']}
    try:
        publish_evidence(path, (json.dumps(payload, indent=2) + '\n').encode('utf-8'),
                         host_record, previous_host=previous_host)
    except OSError as write_error:
        print(f'run_qualification: cannot write {path}: {write_error}', file=sys.stderr)
    print(f'run_qualification: unexpected error: {message}', file=sys.stderr)
    return 1


def build_parser():
    parser = argparse.ArgumentParser(
        description='Run render qualification cells and write qualification.json.')
    parser.add_argument('--client', required=True,
                        help='Render benchmark client executable; a path ending in .py is '
                             'run with the current interpreter (used by tests)')
    parser.add_argument('--out-dir', required=True, type=Path,
                        help='Directory receiving cells/, work/ and qualification.json')
    parser.add_argument('--runtime-root', type=Path,
                        help='Absolute runtime asset directory; otherwise search only '
                             'the selected client executable ancestors')
    parser.add_argument('--matrix', type=Path, default=planner.DEFAULT_MATRIX,
                        help='Qualification matrix JSON (default: committed matrix)')
    parser.add_argument('--build-manifest', type=Path,
                        help='performance-build.json extended with client.sha256; '
                             'required for native qualification')
    parser.add_argument('--host-record', type=Path,
                        help='Staged Windows host JSON; validated before execution and '
                             'published with qualification.json under the output lock')
    parser.add_argument('--only', action='append', metavar='CELL_ID',
                        help='Select one cell id; repeatable; makes the run partial')
    parser.add_argument('--timeout-seconds', type=int, default=DEFAULT_TIMEOUT_SECONDS,
                        help='Client process timeout (default: 900)')
    parser.add_argument('--declare-refresh', action='append', default=[],
                        metavar='DISPLAY_ID=HZ',
                        help='Operator refresh rate for a display the matrix leaves null')
    parser.add_argument('--checker', type=Path, default=None,
                        help='Checker script run in place of validate_render_capture.py')
    parser.add_argument('--dry-run', action='store_true',
                        help='Write qualification-plan.json and launch nothing')
    parser.add_argument('--format', choices=('json',), default='json',
                        help='Output format (only json)')
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    try:
        if args.timeout_seconds < 1:
            raise UsageError('--timeout-seconds must be at least 1')
        matrix = matrix_lib.load_matrix(args.matrix)
        provenance = matrix_identity(matrix, args.matrix)
        build_manifest = load_build_manifest(args.build_manifest)
        host_record = load_host_record(args.host_record)
        declared = parse_declarations(args.declare_refresh, matrix)
        planner.select(matrix_lib.cells(matrix), args.only)
    except (matrix_lib.MatrixError, UsageError) as error:
        print(f'run_qualification: {error}', file=sys.stderr)
        return 2
    out_dir = runner.resolve_path(args.out_dir)
    client = resolve_client(str(args.client))
    checker = None if args.checker is None else resolve_checker(str(args.checker))
    lock = out_dir / LOCK_NAME
    try:
        out_dir.mkdir(parents=True, exist_ok=True)
    except OSError as error:
        print(f'run_qualification: cannot write under {out_dir}: {error}', file=sys.stderr)
        return 2
    try:
        descriptor = os.open(lock, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except OSError as error:
        if isinstance(error, FileExistsError) and os.path.lexists(lock):
            print(f'run_qualification: output directory locked: {lock}; '
                  'no automatic stale-lock reclaim', file=sys.stderr)
        else:
            print(f'run_qualification: cannot write under {out_dir}: {error}', file=sys.stderr)
        return 2
    try:
        os.close(descriptor)
        if args.dry_run:
            try:
                code = run_dry(matrix, args, out_dir)
                if host_record is not None:
                    runner.atomic_write(out_dir / 'host-dryrun.json', host_record)
                return code
            except (OSError, UsageError) as error:
                print(f'run_qualification: {error}', file=sys.stderr)
                return 2
        return run_locked(matrix, args, out_dir, client, checker, declared,
                          provenance, build_manifest, host_record)
    finally:
        lock.unlink()


def run_locked(matrix, args, out_dir, client, checker, declared, provenance, build_manifest,
               host_record=None):
    """Execute and atomically publish while main owns the directory lock."""
    path = out_dir / QUALIFICATION_NAME
    try:
        previous_host = read_host_bytes(out_dir / 'host.json', missing_ok=True)
    except UsageError as error:
        print(f'run_qualification: {error}', file=sys.stderr)
        return 2
    try:
        set_aside(path)
        results = run_cells(matrix, client=client, out_dir=out_dir,
                            only=args.only, timeout_seconds=args.timeout_seconds,
                            checker=checker, matrix_canonical=provenance['canonical'],
                            build_manifest=build_manifest, runtime_root=args.runtime_root)
        payload = aggregate(matrix, results, partial=args.only is not None,
                            declared_refresh=declared, matrix_provenance=provenance)
    except Exception as error:  # noqa: BLE001 - Any failure must leave an unqualified record.
        return fail_closed(path, error, host_record, previous_host=previous_host)
    try:
        publish_evidence(path, (json.dumps(payload, indent=2) + '\n').encode('utf-8'),
                         host_record, previous_host=previous_host)
    except OSError as error:
        print(f'run_qualification: cannot write {path}: {error}', file=sys.stderr)
        return 2
    summary = payload['summary']
    print(f"run_qualification: {len(results)} cells, pass {summary['pass']}, "
          f"fail {summary['fail']}, invalid {summary['invalid']}, "
          f"process_failed {summary['process_failed']}, "
          f"unevaluated {summary['unevaluated']}, "
          f"render_floor_qualified {str(payload['render_floor_qualified']).lower()} "
          f"-> {path}")
    return exit_code(summary)


if __name__ == '__main__':
    sys.exit(main())
