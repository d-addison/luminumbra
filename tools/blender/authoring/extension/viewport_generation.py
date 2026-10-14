"""Background metadata reader for immutable, service-published static prefabs.

GenerationReader(project, asset_id, generation_id='') follows current.json or an
explicit generation. poll() and status() only copy bounded in-memory snapshots;
they never read files or call Blender. A result describes its exact generation,
not the most recently requested one. Errors clear the result. The native host
must still validate every generation member and the full prefab contract.
"""
import copy
import hashlib
import json
import math
from pathlib import Path
import re
import stat
import threading

from .viewport.protocol import open_record_reader, record_file_identity, record_reader_identity


GENERATION = 'luminumbra.authoring.generation.v1'
PREFAB = 'luminumbra.asset.prefab.v1'
IDENTIFIER = re.compile(r'[A-Za-z][A-Za-z0-9_.:-]{0,127}\Z')
HEX32 = re.compile(r'[0-9a-f]{32}\Z')
HEX64 = re.compile(r'[0-9a-f]{64}\Z')
MEMBER = re.compile(r'(?:m-[0-9a-f]{32}\.lmesh|t-[0-9a-f]{32}\.ltex)\Z')
MAX_DOCUMENT = 1024 * 1024
MAX_OUTPUT = 256 * 1024 * 1024
MAX_NODES = 1024
MAX_PINS = 64
POLL_SECONDS = .1
CLOSE_SECONDS = .25


class MetadataError(ValueError):
    pass


def _require(condition, message):
    if not condition:
        raise MetadataError(message)


def _matches(pattern, value):
    return type(value) is str and pattern.fullmatch(value) is not None


def _unsigned(value, maximum=(1 << 64) - 1):
    return type(value) is int and 0 <= value <= maximum


def _fields(value, required, optional=()):
    _require(type(value) is dict and set(required) <= value.keys() and
             value.keys() <= set(required) | set(optional), 'Unsupported metadata fields')


def _document(raw):
    def unique(pairs):
        result = {}
        for key, value in pairs:
            _require(key not in result, 'Duplicate metadata field')
            result[key] = value
        return result

    def invalid(_):
        raise MetadataError('Nonfinite metadata number')

    value = json.loads(raw.decode('utf-8'), object_pairs_hook=unique, parse_constant=invalid)
    # Also reject overflowing exponents in otherwise unused descriptor fields.
    json.dumps(value, allow_nan=False)
    _require(type(value) is dict, 'Metadata must be an object')
    return value


def _checked_stat(path, directory):
    info = path.lstat()
    _require(not stat.S_ISLNK(info.st_mode) and
             not getattr(info, 'st_file_attributes', 0) & 0x400,
             'Metadata paths cannot contain symlinks or reparse points')
    _require(stat.S_ISDIR(info.st_mode) if directory else stat.S_ISREG(info.st_mode),
             'Metadata requires regular files and directories')
    return info


def _read_regular(path, limit):
    # Check the project ancestors as well: resolving first would hide links.
    ancestors = list(reversed(path.parents))
    before = [_checked_stat(part, True) for part in ancestors]
    info = _checked_stat(path, False)
    _require(0 < info.st_size <= limit, 'Metadata file exceeds its byte bound')
    identity = record_file_identity(path)
    _require(identity.size == info.st_size, 'Metadata file changed before opening')
    # Compare the same strong identity API before opening, while reading, and
    # after reading. Windows path/descriptor ctime may mean different things;
    # native change time remains part of every identity comparison.
    with open_record_reader(path) as stream:
        _require(record_reader_identity(stream) == identity,
                 'Metadata file changed while opening')
        raw = stream.read(limit + 1)
        _require(0 < len(raw) == info.st_size <= limit and
                 record_reader_identity(stream) == identity,
                 'Metadata file changed while reading')
    _checked_stat(path, False)
    _require(record_file_identity(path) == identity,
             'Metadata file was replaced while reading')
    for part, previous in zip(ancestors, before):
        current = _checked_stat(part, True)
        # Other directory entries may change during publication; identity must not.
        _require((current.st_dev, current.st_ino) == (previous.st_dev, previous.st_ino),
                 'Metadata directory was replaced while reading')
    return raw


def _matrix(value):
    _require(type(value) is list and len(value) == 16 and
             all(type(x) in (int, float) and math.isfinite(x) and abs(x) <= 1e30 for x in value),
             'Invalid prefab local matrix')
    _require([value[i] for i in (3, 7, 11, 15)] == [0, 0, 0, 1],
             'Prefab local matrix must be affine')
    m = value
    determinant = (m[0] * (m[5]*m[10] - m[9]*m[6]) -
                   m[4] * (m[1]*m[10] - m[9]*m[2]) +
                   m[8] * (m[1]*m[6] - m[5]*m[2]))
    _require(abs(determinant) > 1e-12, 'Prefab local matrix is singular')
    return list(value)


def _nodes(descriptor):
    source = descriptor['nodes']
    _require(type(source) is list and 1 <= len(source) <= MAX_NODES,
             'Viewport prefab requires 1..1024 nodes')
    nodes, parents = [], {}
    for node in source:
        _fields(node, ('id', 'parent', 'local_matrix', 'label', 'reverse_front_face'), ('mesh',))
        identity, parent = node['id'], node['parent']
        _require(_matches(IDENTIFIER, identity) and identity not in parents,
                 'Invalid or duplicate prefab node ID')
        _require(parent is None or _matches(IDENTIFIER, parent), 'Invalid prefab parent ID')
        _require(type(node['label']) is str and type(node['reverse_front_face']) is bool and
                 ('mesh' not in node or _matches(IDENTIFIER, node['mesh'])),
                 'Invalid prefab node metadata')
        parents[identity] = parent
        nodes.append({'id': identity, 'parent': parent, 'local_matrix': _matrix(node['local_matrix'])})
    # Iterative, linear parent walks also admit the complete 1024-node chain.
    done = set()
    for identity in parents:
        visiting = set()
        while identity is not None and identity not in done:
            _require(identity in parents, 'Missing prefab parent')
            _require(identity not in visiting, 'Cyclic prefab parent graph')
            visiting.add(identity)
            identity = parents[identity]
        done.update(visiting)
    return nodes


def _read_metadata(project, asset_id, generation_id, pins):
    root = project / '.luminumbra-author'
    pointer_raw = None
    if not generation_id:
        pointer_raw = _read_regular(root / 'current.json', 65536)
        pointer = _document(pointer_raw)
        _fields(pointer, ('schema', 'job_id', 'build_identity'))
        _require(pointer['schema'] == GENERATION and _matches(HEX32, pointer['job_id']) and
                 _matches(HEX64, pointer['build_identity']), 'Invalid current generation pointer')
        generation_id = pointer['job_id']
    directory = root / 'generations' / generation_id
    raw = _read_regular(directory / 'manifest.json', MAX_DOCUMENT)
    manifest_hash = hashlib.sha256(raw).hexdigest()
    _require(generation_id not in pins or pins[generation_id] == manifest_hash,
             'Immutable generation manifest changed')
    _require(generation_id in pins or len(pins) < MAX_PINS, 'Viewport generation session bound (64)')
    manifest = _document(raw)
    _fields(manifest, ('schema', 'mode', 'job_id', 'asset_id', 'revision',
                       'build_identity', 'input_hashes', 'outputs'))
    _require(manifest['schema'] == GENERATION and manifest['mode'] == 'NATIVE' and
             manifest['job_id'] == generation_id and manifest['asset_id'] == asset_id and
             _unsigned(manifest['revision']) and _matches(HEX64, manifest['build_identity']),
             'Generation schema, revision, or asset identity mismatch')
    if pointer_raw is not None:
        _require(pointer['build_identity'] == manifest['build_identity'],
                 'Current generation build identity mismatch')
    inputs = manifest['input_hashes']
    _require(type(inputs) is dict and 1 <= len(inputs) <= 130 and
             all(_matches(HEX64, value) for value in inputs.values()), 'Invalid source hash metadata')
    outputs = manifest['outputs']
    _require(type(outputs) is dict and 2 <= len(outputs) <= 129 and 'prefab.json' in outputs,
             'Invalid prefab output inventory')
    total = 0
    for name, entry in outputs.items():
        _require(name == 'prefab.json' or _matches(MEMBER, name), 'Invalid prefab output path')
        _require(type(entry) is dict and _matches(HEX64, entry.get('sha256')) and
                 _unsigned(entry.get('bytes'), MAX_OUTPUT) and entry['bytes'] > 0,
                 'Invalid prefab output hash or byte count')
        total += entry['bytes']
    _require(total <= MAX_OUTPUT, 'Prefab output byte budget exceeded')
    entry = outputs['prefab.json']
    _fields(entry, ('format', 'schema', 'nodes', 'meshes', 'materials', 'bytes', 'sha256'))
    _require(entry['format'] == 'PREFAB' and entry['schema'] == PREFAB and
             _unsigned(entry['nodes'], MAX_NODES) and entry['nodes'] > 0 and
             _unsigned(entry['meshes'], 128) and _unsigned(entry['materials'], 4096),
             'Unsupported prefab descriptor metadata')
    raw = _read_regular(directory / 'prefab.json', MAX_DOCUMENT)
    _require(len(raw) == entry['bytes'] and hashlib.sha256(raw).hexdigest() == entry['sha256'],
             'Prefab descriptor hash or byte count mismatch')
    descriptor = _document(raw)
    _fields(descriptor, ('schema', 'asset_id', 'coordinates', 'nodes',
                         'meshes', 'materials', 'runtime_components'))
    _require(descriptor['schema'] == PREFAB and descriptor['asset_id'] == asset_id and
             descriptor['coordinates'] == 'gltf-rh-y-up-meters' and
             descriptor['runtime_components'] is False, 'Unsupported prefab descriptor identity')
    nodes = _nodes(descriptor)
    _require(len(nodes) == entry['nodes'] and type(descriptor['meshes']) is dict and
             len(descriptor['meshes']) == entry['meshes'] and type(descriptor['materials']) is dict and
             len(descriptor['materials']) == entry['materials'], 'Prefab descriptor counts disagree')
    # Bind the whole read to one publication; never pair B's label with A's nodes.
    if pointer_raw is not None:
        _require(_read_regular(root / 'current.json', 65536) == pointer_raw,
                 'Current generation changed during metadata read')
    _require(hashlib.sha256(_read_regular(directory / 'manifest.json', MAX_DOCUMENT)).hexdigest() ==
             manifest_hash, 'Generation manifest changed during metadata read')
    return {'generation_id': generation_id, 'manifest_sha256': manifest_hash,
            'revision': manifest['revision'], 'asset_id': asset_id, 'nodes': nodes}


class GenerationReader:
    """One reader thread, one current result, and at most 64 immutable pins.

    close() returns within one 250 ms join. A filesystem call already blocked in
    the OS may outlive that join on the daemon thread; it cannot publish again.
    No project files are created, modified, or deleted by this reader.
    """
    def __init__(self, project, asset_id, generation_id=''):
        _require(_matches(IDENTIFIER, asset_id), 'Invalid asset ID')
        _require(generation_id == '' or _matches(HEX32, generation_id), 'Invalid generation ID')
        project = Path(project)
        _require('..' not in project.parts, 'Project path cannot traverse parents')
        self._project = project.absolute()  # Lexical only; all filesystem validation is on the worker.
        self._asset_id, self._generation_id = asset_id, generation_id
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._result = None
        self._status = {'state': 'loading', 'error': '', 'generation_id': '', 'checks': 0}
        self._pins = {}
        self._thread = threading.Thread(target=self._run, name='Luminumbra generation metadata', daemon=True)
        try:
            self._thread.start()
        except Exception:
            self._stop.set()
            self._status.update(state='closed')
            raise

    def _run(self):
        while not self._stop.is_set():
            try:
                result = _read_metadata(self._project, self._asset_id, self._generation_id, self._pins)
                error = ''
            except Exception as failure:
                result = None
                error = (str(failure) if isinstance(failure, ValueError) else
                         'Metadata read failed: ' + type(failure).__name__)[:256]
            with self._lock:
                if self._stop.is_set():
                    return
                if result is not None:
                    self._pins[result['generation_id']] = result['manifest_sha256']
                self._result = result
                self._status = {'state': 'ready' if result is not None else 'error', 'error': error,
                                'generation_id': result['generation_id'] if result else '',
                                'checks': self._status['checks'] + 1}
            if self._stop.wait(POLL_SECONDS):
                return

    def poll(self):
        with self._lock:
            return copy.deepcopy(self._result)

    def status(self):
        with self._lock:
            return dict(self._status)

    def close(self):
        self._stop.set()
        with self._lock:
            self._result = None
            self._status.update(state='closed', error='', generation_id='')
        if self._thread.ident is not None and self._thread is not threading.current_thread():
            self._thread.join(CLOSE_SECONDS)
