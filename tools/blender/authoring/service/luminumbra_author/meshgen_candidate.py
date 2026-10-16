"""Durable sealed candidate/receipt transactions and append-only attestations.

Mutation runs under a separate short storage guard, inside the MG2 owner's
serialized commit callback. Read-only access never constructs BuildService.
"""
from contextlib import contextmanager, ExitStack
import json
import os
from pathlib import Path
import re
import stat
import threading
import uuid

from . import file_io
from .contracts import Refusal, canonical, digest, require, path_is_link
from .locking import ProjectLock
from .meshgen_backend import bounded_json, validate_capability, validate_result
from .meshgen_config import canonical_relative, verify_effective_config
from .meshgen_contracts import (STATE_ROOT, SEALED_REQUIRED, SEALED_OPTIONAL, CANDIDATE,
                               validate_manifest, validate_provenance, validate_receipt)
from .meshgen_jobs import durable_replace_directory, reject_secret, configured_tokens
from .meshgen_metrics import inspect_artifact
from .meshgen_provenance import make_receipt, verified_policy_notes
from .meshgen_scripts import (_posix_open, _windows_directory, _windows_path_api,
                             _windows_final_path, _windows_long_path)

MAX_JSON = 1024**2
MAX_MEMBERS = 20
MAX_ATTESTATIONS = 256
_MEMBER_LIMITS = {'candidate.glb': 256*1024**2, 'inputs/raw_backend.glb': 256*1024**2,
                  'inputs/reference_image': 16*1024**2, 'preview.png': 16*1024**2,
                  'inputs/backend.log': 16384, 'inputs/finish.log': 16384, 'inputs/prompt.txt': 4096}


def read_member(root, name, limit):
    """Containment and content validation use the same no-follow regular handle."""
    canonical_relative(name)
    root = Path(root).absolute()
    try:
        with ExitStack() as stack:
            if os.name == 'posix':
                root_fd = _posix_open(root)
                stack.callback(os.close, root_fd)
                fd = _posix_open(Path(name), root_fd, directory=False)
                try:
                    info = os.fstat(fd)
                    require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, 'candidate.path', 'Member must be a single-link regular file.')
                    stream = os.fdopen(fd, 'rb')
                except BaseException:
                    os.close(fd); raise
                stream = stack.enter_context(stream)
            else:
                # Retain non-delete-sharing ancestors through open and read.
                path = Path(root.anchor)
                for part in ('', *root.parts[1:], *Path(name).parts[:-1]):
                    path /= part
                    handle, _ = _windows_directory(path)
                    stack.callback(_windows_path_api().CloseHandle, handle)
                stream = stack.enter_context(file_io.open_regular_reader(root/name))
                import msvcrt
                require(_windows_final_path(msvcrt.get_osfhandle(stream.fileno())) == _windows_long_path(root/name),
                        'candidate.path', 'Member handle escaped its directory.')
                require(os.fstat(stream.fileno()).st_nlink == 1, 'candidate.path', 'Hard-linked members are refused.')
            before = file_io.reader_identity(stream)
            require(before.size <= limit, 'candidate.limit', 'Member exceeds byte cap.')
            data = stream.read(limit+1)
            require(len(data) <= limit and before == file_io.reader_identity(stream), 'candidate.changed', 'Member changed during its read.')
            return data
    except (OSError, ValueError) as error:
        if isinstance(error, Refusal): raise
        raise Refusal('candidate.path', 'Cannot safely open candidate member.') from error


def _json(data): return bounded_json(data, max_bytes=MAX_JSON)


def _licence_notes(members, policy):
    encoded = _json(members.get('inputs/licence_notes.json', b'{}'))
    require(isinstance(encoded, dict) and all(isinstance(value, str) and len(value) % 2 == 0
            and re.fullmatch(r'[0-9a-f]*', value) for value in encoded.values()),
            'provenance.inconsistent', 'Malformed retained policy note bytes.')
    # Decoded notes are bounded by the 1 MiB encoded JSON member.
    return verified_policy_notes(policy, {key: bytes.fromhex(value) for key, value in encoded.items()})


def _record_bytes(value):
    # Keep the assembled record, including its newline, within the 1 MiB cap.
    buffer = bytearray()
    encoder = json.JSONEncoder(sort_keys=True, separators=(',', ':'), ensure_ascii=False, allow_nan=False)
    for piece in encoder.iterencode(value):
        chunk = piece.encode('utf-8')
        require(len(chunk) <= MAX_JSON-1-len(buffer), 'candidate.limit', 'Attestation record exceeds byte cap.')
        buffer.extend(chunk)
    buffer.append(10)
    data = bytes(buffer)
    _json(data)
    return data


def validate_evidence(members):
    """Cross-bind every identity/policy field; never infer permission from a boolean."""
    require(SEALED_REQUIRED <= members.keys() and members.keys() <= SEALED_REQUIRED | SEALED_OPTIONAL,
            'candidate.members', 'Candidate members differ from registered layout.')
    p = validate_provenance(_json(members['provenance.json']))
    c = p['config']
    effective = verify_effective_config(members['effective_config.json'], c['effective_config_sha256'], c['config_digest'])
    require(len(members['candidate.glb']) <= effective['limits']['result_bytes']
            and len(members['inputs/raw_backend.glb']) <= effective['limits']['result_bytes']
            and all(len(members.get(n, b'')) <= effective['limits']['log_bytes'] for n in ('inputs/backend.log', 'inputs/finish.log')),
            'candidate.limit', 'Retained bytes exceed resolved caps.')
    card = _json(members['inputs/capabilities.json'])
    require(validate_capability(card, p['backend']['id'], effective['backends'][p['backend']['id']]) == p['backend']['capability_digest'],
            'provenance.inconsistent', 'Capability identity differs.')
    # Independently recompute mock policy. Real backend provenance remains unavailable.
    from .meshgen_licence import gate
    require(p['backend']['id'] == 'mock', 'backend.unavailable', 'Real backend candidates are unavailable.')
    licence = _json(members['licence_gate.json'])
    require(gate(card, effective['policy']['licence'], _licence_notes(members, effective['policy']['licence'])) == licence and licence['runnable'],
            'provenance.inconsistent', 'Licence record differs from trusted parent policy.')
    require({k: licence[k] for k in ('status', 'shippable', 'components', 'dataset_chain_audited', 'ack_ids')}
            == {k: p['licence_gate'][k] for k in ('status', 'shippable', 'components', 'dataset_chain_audited', 'ack_ids')},
            'provenance.inconsistent', 'Provenance licence summary differs.')
    shape = next(c for c in card['components'] if c['role'] == 'shape')
    lic = shape['licence']
    require(p['model'] == {'name': shape['name'], 'repo': shape.get('repo', ''), 'revision': shape['revision'],
                'weights_sha256': shape.get('weights_sha256', []), 'precision': shape.get('precision'),
                'code_commit': card['backend']['adapter_commit'], 'backend_reported': True}
            and {k: p['backend'][k] for k in card['backend']} == card['backend']
            and p['license'] == {'id': lic['id'], 'text_sha256': lic['text_sha256'], 'url': lic.get('url', ''),
                'territory_restricted': 'territory_restricted' in lic['clauses'], 'user_acknowledgment': lic['id'] in licence['ack_ids']},
            'provenance.inconsistent', 'Inherited model/licence identity differs from retained capability.')
    pairs = [('candidate.glb', p['artifact']['sha256']), ('inputs/raw_backend.glb', p['raw_output']['sha256']),
             ('inputs/reference_image', p['request']['image_sha256']),
             ('stats.json', p['metrics']['stats_sha256']), ('licence_gate.json', p['licence_gate']['sha256'])]
    require(all(digest(members[name]) == expected for name, expected in pairs), 'provenance.inconsistent', 'Retained byte identities differ.')
    from .meshgen_jobs import inspect_raw_glb
    raw = inspect_raw_glb(members['inputs/raw_backend.glb'], effective)
    require(raw == {k: p['raw_output'][k] for k in ('triangles', 'vertices')}
            and len(members['inputs/raw_backend.glb']) == p['raw_output']['bytes'], 'provenance.inconsistent', 'Raw output observations differ.')
    result = validate_result(_json(members['inputs/result.json']))
    require(result['sha256'] == p['raw_output']['sha256'] and result['bytes'] == p['raw_output']['bytes']
            and result['backend_reported']['seed'] == p['request']['seed'] and result['backend_reported']['peak_vram_mib'] is None, 'provenance.inconsistent', 'Result metadata differs.')
    actual = inspect_artifact(members['candidate.glb'], effective)
    require({k: actual[k] for k in p['artifact'] if k != 'sha256'} == {k: v for k, v in p['artifact'].items() if k != 'sha256'}
            and actual['has_normals'], 'provenance.inconsistent', 'Artifact statistics differ.')
    stats = _json(members['stats.json'])
    from .meshgen_provenance import validate_observations
    require(isinstance(stats, dict) and set(stats) == {'protocol', 'artifact', 'guard', 'gates', 'observations', 'guard_observation', 'acceptance', 'simulated'}
            and stats['protocol'] == 'luminumbra.meshgen.stats.v1'
            and type(stats['simulated']) is bool and stats['simulated'] == (p['finish']['blender_version'] == 'simulated'),
            'provenance.inconsistent', 'Malformed statistics envelope.')
    validate_observations({'finish': stats['observations'], 'guard': stats['guard_observation']}, effective)
    require(stats['artifact'] == actual and stats['observations'] == p['finish'], 'provenance.inconsistent', 'Stored statistics differ.')
    from .meshgen_provenance import quality_gates
    from .meshgen_metrics import guard_decision, validate_png
    from .meshgen_contracts import accepted_waivers
    guard = guard_decision(stats.get('guard_observation', {}), effective['finish']['decimate'])
    expected_gates = quality_gates(actual, guard, effective)
    require(stats['guard'] == guard and stats['gates'] == expected_gates, 'provenance.inconsistent', 'Gate decisions differ from observations.')
    validate_png(members['preview.png'], effective['limits'])
    require(len(members['inputs/reference_image']) <= effective['limits']['image_bytes'], 'candidate.limit', 'Image exceeds resolved cap.')
    failed = sorted(k for k, outcome in stats['gates'].items() if outcome == 'fail')
    require(accepted_waivers(failed, effective, stats.get('acceptance')) == p['flags']['waived'],
            'provenance.inconsistent', 'Recorded human acceptance differs from waivers.')
    require(failed == sorted(p['flags']['waived']) and (not failed or
            effective['policy']['validation_profile'] == 'internal' and set(failed) <= set(effective['evaluation']['waivable'])),
            'provenance.inconsistent', 'Failed gates differ from committed waivers.')
    keep_prompt = effective['logging']['keep_prompts'] == 'text'
    require(('inputs/prompt.txt' in members) == keep_prompt and ('prompt_text' in p['request']) == keep_prompt,
            'provenance.inconsistent', 'Prompt retention differs from project policy.')
    if keep_prompt:
        require(members['inputs/prompt.txt'] == p['request']['prompt_text'].encode()
                and digest(members['inputs/prompt.txt']) == p['request']['prompt_digest'], 'provenance.inconsistent', 'Prompt evidence differs.')
    return p, stats


class RetainedDirectories:
    """Pin every ancestor through mutation; verify retained identities afterward.

    POSIX mutations use directory descriptors exclusively. Windows directories
    deny delete sharing and reject reparse points, preventing ancestor renames.
    Staging is outside every candidate seal, including directory staging.
    """
    def __init__(self, root):
        self.root = root
        self.stack = ExitStack()
        self.directories = {}

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.stack.close()

    def verify(self):
        for path, (handle, identity) in self.directories.items():
            if os.name == 'posix':
                if path == Path(path.anchor):
                    info = os.stat(path, follow_symlinks=False)
                else:
                    parent = self.directories[path.parent][0]
                    info = os.stat(path.name, dir_fd=parent, follow_symlinks=False)
                require(stat.S_ISDIR(info.st_mode) and (info.st_dev, info.st_ino) == identity,
                        'candidate.path', 'Publication ancestor identity changed.')
            else:
                require(_windows_final_path(handle) == _windows_long_path(path),
                        'candidate.path', 'Publication ancestor moved.')

    def directory(self, path):
        path = Path(path).absolute()
        require('..' not in path.parts, 'candidate.path', 'Publication traversal refused.')
        if path in self.directories:
            self.verify()
            return self.directories[path][0]
        if path != Path(path.anchor):
            parent = self.directory(path.parent)
        if os.name == 'posix':
            flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
            try:
                handle = os.open(path.name, flags, dir_fd=parent) if path != Path(path.anchor) else os.open(path, flags)
            except FileNotFoundError:
                require((path.is_relative_to(self.root) or path in self.root.parents), 'candidate.path', 'Cannot create outside state.')
                os.mkdir(path.name, 0o700, dir_fd=parent)
                os.fsync(parent)
                handle = os.open(path.name, flags, dir_fd=parent)
            self.stack.callback(os.close, handle)
            info = os.fstat(handle)
            identity = info.st_dev, info.st_ino
        else:
            try:
                handle, identity = _windows_directory(path)
            except FileNotFoundError:
                require((path.is_relative_to(self.root) or path in self.root.parents), 'candidate.path', 'Cannot create outside state.')
                # A first-level directory temp is already outside all seals.
                staging = self.root/'.candidate-staging'
                if path == self.root or path == staging or not staging.exists():
                    temporary = path.parent/('.mkdir-'+uuid.uuid4().hex)
                else:
                    self.directory(staging)
                    temporary = staging/('.mkdir-'+uuid.uuid4().hex)
                temporary.mkdir(mode=0o700)
                try:
                    self.verify()
                    durable_replace_directory(temporary, path)
                finally:
                    if temporary.exists(): temporary.rmdir()
                handle, identity = _windows_directory(path)
            self.stack.callback(_windows_path_api().CloseHandle, handle)
        self.directories[path] = handle, identity
        self.verify()
        return handle

    def write(self, path, data):
        require(path.is_relative_to(self.root) and '..' not in path.parts,
                'candidate.path', 'Publication must remain in owned state.')
        staging = self.root/'.candidate-staging'
        stage_fd = self.directory(staging)
        parent = self.directory(path.parent)
        temporary = '.publish-'+uuid.uuid4().hex
        if os.name == 'posix':
            fd = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                         0o600, dir_fd=stage_fd)
            try:
                with os.fdopen(fd, 'wb') as stream:
                    stream.write(data); stream.flush(); os.fsync(stream.fileno())
                    self.verify()
                    file_io.replace_file(temporary, path.name, durable=True,
                                         source_dir_fd=stage_fd, target_dir_fd=parent)
                    installed = os.stat(path.name, dir_fd=parent, follow_symlinks=False)
                    retained = os.fstat(stream.fileno())
                    require((installed.st_dev, installed.st_ino) == (retained.st_dev, retained.st_ino),
                            'candidate.path', 'Publication temporary identity changed.')
                    os.fsync(parent)
                    if parent != stage_fd: os.fsync(stage_fd)
                    self.verify()
            finally:
                try: os.unlink(temporary, dir_fd=stage_fd)
                except FileNotFoundError: pass
        else:
            temporary = staging/temporary
            try:
                with temporary.open('xb') as stream:
                    stream.write(data); stream.flush(); os.fsync(stream.fileno())
                self.verify()
                file_io.replace_file(temporary, path, durable=True, allow_cross_directory=True)
                self.verify()
            finally:
                temporary.unlink(missing_ok=True)


class CandidateStore:
    def __init__(self, project, document='_headless', *, tokens=(), secret_source=None):
        self.project = Path(project).absolute()
        require(re.fullmatch(r'[A-Za-z0-9_-]{1,64}', document) is not None, 'candidate.path', 'Invalid document identifier.')
        self.root = self.project/STATE_ROOT/document
        self.tokens = tuple(tokens)
        self.secret_source = secret_source or (lambda: configured_tokens(self.project))
        self._reject(document)
        self.mutex = threading.RLock()

    def _reject(self, *payloads):
        for token in (*self.tokens, *self.secret_source()): reject_secret(token, *payloads)

    def paths(self, job):
        require(isinstance(job, str) and re.fullmatch(r'[0-9a-f]{32}', job), 'candidate.path', 'Invalid candidate identifier.')
        self._reject(job)
        return self.root/'candidates'/job, self.root/'jobs'/job

    def _safe(self, path):
        try: return file_io.safe_path(path)
        except ValueError as error: raise Refusal('candidate.path', 'Reparse candidate path.') from error

    @contextmanager
    def _publication(self):
        try:
            with RetainedDirectories(self.root) as directories:
                directories.directory(self.project)
                yield directories
        except (OSError, ValueError) as error:
            if isinstance(error, Refusal): raise
            if isinstance(error, OSError) and error.errno not in (2, 20, 40): raise
            raise Refusal('candidate.path', 'Cannot retain publication ancestors.') from error

    @contextmanager
    def _guard(self):
        with self.mutex, self._publication() as directories:
            root_fd = directories.directory(self.root)
            directories.directory(self.root/'jobs')
            if os.name == 'posix':
                import fcntl
                fd = os.open('.candidate.guard', os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW,
                             0o600, dir_fd=root_fd)
                guard = os.fdopen(fd, 'a+b')
                try: fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError as error:
                    guard.close()
                    raise Refusal('busy', 'Candidate storage is busy.') from error
            else:
                try: guard = ProjectLock(self.root/'candidate-storage', self.root/'.candidate.guard')
                except BlockingIOError as error: raise Refusal('busy', 'Candidate storage is busy.') from error
            self._directories = directories
            try:
                directories.verify()
                yield
                directories.verify()
            finally:
                self._directories = None
                guard.close()

    def _write(self, path, data, decoded=None):
        require(type(data) is bytes, 'candidate.members', 'Publication requires bytes.')
        require(not path.name.endswith('.json') or len(data) <= MAX_JSON, 'candidate.limit', 'JSON publication exceeds cap.')
        self._reject(str(path), data, decoded)
        directories = getattr(self, '_directories', None)
        if directories is not None:
            try: directories.write(path, data)
            except (OSError, ValueError) as error:
                if isinstance(error, Refusal): raise
                if isinstance(error, OSError) and error.errno not in (2, 20, 40): raise
                raise Refusal('candidate.path', 'Publication ancestor changed.') from error
        else:
            with self._publication() as directories: directories.write(path, data)

    def _quota(self, incoming, byte_limit):
        state = self.project/STATE_ROOT
        used, count, pending = 0, 0, [state]
        while pending:
            directory = pending.pop()
            self._safe(directory)
            with os.scandir(directory) as entries:
                for entry in entries:
                    count += 1
                    require(count <= 10000, 'candidate.limit', 'Project retained entry quota is full.')
                    info = entry.stat(follow_symlinks=False)
                    require(not path_is_link(Path(entry.path), info), 'candidate.path', 'Retained state cannot use reparse paths.')
                    if stat.S_ISDIR(info.st_mode): pending.append(Path(entry.path))
                    else:
                        require(stat.S_ISREG(info.st_mode), 'candidate.path', 'Retained state requires regular files.')
                        used += info.st_size
                    require(used+incoming <= byte_limit, 'candidate.limit', 'Project retained byte quota is full.')

    def publish(self, job, members):
        """Use as the E3 ledger.commit_success callback; its mutex serializes cancel."""
        with self._guard():
            candidate, area = self.paths(job)
            self._safe(candidate); self._safe(area)
            require(not candidate.exists() and not (area/'receipt.prepared.json').exists(), 'request.conflict', 'Candidate identity is already reserved.')
            for name, data in members.items():
                canonical_relative(name)
                require(type(data) is bytes and len(data) <= _MEMBER_LIMITS.get(name, MAX_JSON), 'candidate.limit', 'Invalid member byte bound.')
                self._reject(name, data, _json(data) if name.endswith('.json') else None)
            provenance, stats = validate_evidence(members)
            effective = _json(members['effective_config.json'])
            self._reject(*_licence_notes(members, effective['policy']['licence']).values())
            self._quota(sum(len(v) for v in members.values())+16384, effective['retention']['max_total_mib']*1024**2)
            if (area/'running.json').exists():
                owner = _json(read_member(area, 'running.json', MAX_JSON))
                context = owner.get('backend_context') or {}
                require(not context or context['config_digest'] == provenance['config']['config_digest']
                        and context['backend_id'] == provenance['backend']['id'], 'receipt.changed', 'Candidate backend/config identity differs from owner.')
                require(owner['request_id'] == provenance['request_id'] and owner['plan_digest'] == provenance['plan_sha256']
                        and owner['state'] == 'finishing' and (not owner.get('backend_job_id') or
                        owner.get('task_drained') is True and owner.get('backend_stopped') is True),
                        'receipt.changed', 'Candidate differs from the stopped, ready owner plan.')
            manifest = {'schema': CANDIDATE, 'members': {k: digest(v) for k, v in sorted(members.items())}}
            validate_manifest(manifest)
            manifest_bytes = canonical(manifest)+b'\n'
            identity = digest(manifest_bytes)
            receipt = make_receipt(provenance, candidate.relative_to(self.project).as_posix(), identity,
                                   digest(members['provenance.json']), stats)
            # Prepare outside the seal before any commit marker. Failures retain evidence.
            self._write(area/'receipt.prepared.json', canonical(receipt)+b'\n', receipt)
            for name, data in members.items(): self._write(candidate/name, data)
            self._write(candidate/'manifest.json', manifest_bytes, manifest)
            # Validate actual bytes, not just the in-memory manifest, before commit.
            self._read_bundle(candidate, identity, committed=False)
            self._write_head(job, {'candidate_id': job, 'seal': identity, 'count': 0, 'head': '0'*64, 'pending': None})
            self._write(candidate/'candidate.sha256', (identity+'\n').encode())
            return self._recover(job)

    def _walk(self, root):
        """Small fixed layout: refuse links/reparse points before traversing."""
        self._safe(root)
        found, pending = set(), [root]
        while pending:
            directory = pending.pop()
            with os.scandir(directory) as entries:
                count = 0
                for entry in entries:
                    count += 1
                    require(count <= MAX_MEMBERS+MAX_ATTESTATIONS*2, 'candidate.limit', 'Directory entry cap exceeded.')
                    path = Path(entry.path)
                    # Windows DirEntry.stat() leaves identity and link count at
                    # zero. lstat supplies the link count without following links.
                    info = os.lstat(path)
                    require(not path_is_link(path, info), 'candidate.path', 'Symlink/reparse candidate member.')
                    name = path.relative_to(root).as_posix()
                    if stat.S_ISDIR(info.st_mode):
                        require(name in ('inputs', 'attestations'), 'candidate.members', 'Unexpected directory.')
                        if name != 'attestations': pending.append(path)
                    else:
                        require(stat.S_ISREG(info.st_mode) and info.st_nlink == 1, 'candidate.path', 'Nonregular/hard-linked candidate member.')
                        found.add(name)
        return found

    def _read_bundle(self, root, identity=None, *, committed=True):
        if committed:
            marker = read_member(root, 'candidate.sha256', 65)
            require(re.fullmatch(rb'[0-9a-f]{64}\n', marker), 'candidate.changed', 'Invalid seal marker.')
            actual_identity = marker[:64].decode()
            require(identity is None or actual_identity == identity, 'candidate.changed', 'Candidate identity differs.')
            identity = actual_identity
        manifest_bytes = read_member(root, 'manifest.json', MAX_JSON)
        require(digest(manifest_bytes) == identity, 'candidate.changed', 'Manifest differs from seal.')
        manifest = validate_manifest(_json(manifest_bytes))
        expected = set(manifest['members']) | {'manifest.json'} | ({'candidate.sha256'} if committed else set())
        require(self._walk(root) == expected, 'candidate.members', 'Missing or extra sealed members.')
        members = {name: read_member(root, name, _MEMBER_LIMITS.get(name, MAX_JSON)) for name in manifest['members']}
        require(all(digest(data) == manifest['members'][name] for name, data in members.items()), 'candidate.changed', 'Sealed member changed.')
        p, stats = validate_evidence(members)
        return {'sha256': identity, 'provenance': p, 'stats': stats, 'members': members}

    def read(self, job, identity=None):
        candidate, _ = self.paths(job)
        self._safe(candidate)
        require((candidate/'candidate.sha256').exists(), 'candidate.incomplete', 'Uncommitted candidates are unavailable.')
        result = self._read_bundle(candidate, identity)
        self.attestations(job)
        return result

    def stage_for_import(self, job, identity, provenance_sha256, destination):
        """Copy verified bytes into new import-private staging; never reopen the GLB.

        G3 binds the remaining scene/session/scope/recipe fields in its plan and
        performs the main-thread transaction. This method performs no scene work.
        """
        destination = Path(destination).absolute()
        require('..' not in destination.parts, 'candidate.path', 'Import snapshot traversal is unsupported.')
        with self._guard():
            candidate, area = self.paths(job)
            require(destination.is_relative_to(area/'import-private') and destination.suffix == '.glb',
                    'candidate.path', 'Import snapshot requires the job-private staging area.')
            canonical_relative(destination.relative_to(area).as_posix())
            self._safe(destination)
            require(not destination.exists(), 'request.conflict', 'Import snapshot already exists.')
            bundle = self.read(job, identity)
            require(digest(bundle['members']['provenance.json']) == provenance_sha256,
                    'candidate.changed', 'Import provenance differs from review.')
            data = bundle['members']['candidate.glb']
            config = bundle['provenance']['config']
            effective = verify_effective_config(bundle['members']['effective_config.json'],
                config['effective_config_sha256'], config['config_digest'])
            self._quota(len(data)+1024, effective['retention']['max_total_mib']*1024**2)
            self._write(destination, data)
            require(digest(read_member(area, destination.relative_to(area).as_posix(), 256*1024**2)) == digest(data),
                    'candidate.changed', 'Import snapshot changed during publication.')
            return {'path': str(destination), 'artifact_sha256': digest(data), 'candidate_sha256': identity}

    def _recover(self, job):
        candidate, area = self.paths(job)
        bundle = self.read(job)
        prepared_bytes = read_member(area, 'receipt.prepared.json', MAX_JSON)
        prepared = validate_receipt(_json(prepared_bytes))
        expected = make_receipt(bundle['provenance'], candidate.relative_to(self.project).as_posix(), bundle['sha256'],
                                digest(bundle['members']['provenance.json']), bundle['stats'])
        require(prepared == expected, 'receipt.changed', 'Prepared receipt differs from the complete seal.')
        for name, data in (('receipt.json', prepared_bytes), ('receipt.sha256', (digest(prepared_bytes)+'\n').encode())):
            path = area/name
            if path.exists():
                require(read_member(area, name, MAX_JSON) == data, 'receipt.changed', 'Published receipt cannot be rewritten.')
            else: self._write(path, data, prepared if name.endswith('.json') else None)
        return prepared

    def recover(self, job):
        with self._guard(): return self._recover(job)

    def receipt(self, job):
        candidate, area = self.paths(job)
        data = read_member(area, 'receipt.json', MAX_JSON)
        require(read_member(area, 'receipt.sha256', 65) == (digest(data)+'\n').encode(), 'receipt.changed', 'Receipt hash differs.')
        value = validate_receipt(_json(data))
        bundle = self.read(job, value['candidate']['sha256'])
        require(value == make_receipt(bundle['provenance'], candidate.relative_to(self.project).as_posix(), bundle['sha256'],
                    digest(bundle['members']['provenance.json']), bundle['stats']), 'receipt.changed', 'Receipt differs from seal.')
        return value

    def _head_path(self, job):
        self.paths(job)
        # Durable project-state pointer beside the E3 ledger; outside both the
        # candidate and the job evidence that can be replaced together.
        return self.root/'attestation-heads'/(job+'.json')

    def _write_head(self, job, head):
        data = canonical(head)+b'\n'
        path = self._head_path(job)
        self._reject(str(path), head, data)
        require(len(data) <= MAX_JSON, 'candidate.limit', 'Attestation pointer exceeds cap.')
        directories = getattr(self, '_directories', None)
        if directories is not None: directories.write(path, data)
        else:
            with self._publication() as directories: directories.write(path, data)

    def _head(self, job, seal):
        path = self._head_path(job)
        try: head = _json(read_member(path.parent, path.name, MAX_JSON))
        except (Refusal, ValueError) as error:
            raise Refusal('attestation.changed', 'Authoritative attestation pointer is unavailable.') from error
        require(isinstance(head, dict) and set(head) == {'candidate_id', 'seal', 'count', 'head', 'pending'}
                and head['candidate_id'] == job and head['seal'] == seal
                and type(head['count']) is int and 0 <= head['count'] <= MAX_ATTESTATIONS
                and isinstance(head['head'], str) and re.fullmatch(r'[0-9a-f]{64}', head['head'])
                and (head['count'] != 0 or head['head'] == '0'*64),
                'attestation.changed', 'Malformed authoritative attestation pointer.')
        return head

    def _attestation_state(self, job):
        candidate, area = self.paths(job)
        anchor = area/'attestations.json'
        if not anchor.exists():
            require(not (candidate/'attestations').exists() and not (area/'attestation-history').exists(), 'attestation.changed', 'Unanchored attestation directory.')
            return {'schema': 'luminumbra.meshgen.attestations.v1', 'entries': [], 'pending': None}
        state = _json(read_member(area, 'attestations.json', MAX_JSON))
        require(isinstance(state, dict) and set(state) == {'schema', 'entries', 'pending'} and state['schema'] == 'luminumbra.meshgen.attestations.v1'
                and isinstance(state['entries'], list) and len(state['entries']) <= MAX_ATTESTATIONS,
                'attestation.changed', 'Malformed attestation anchor.')
        return state

    def attestations(self, job):
        self.paths(job)
        try: return self._attestations(job)
        except (Refusal, OSError, ValueError, KeyError, TypeError) as error:
            if isinstance(error, Refusal) and error.finding['rule_id'] == 'attestation.changed': raise
            raise Refusal('attestation.changed', 'Cannot validate retained attestation history.') from error

    def _attestations(self, job):
        candidate, area = self.paths(job)
        state = self._attestation_state(job)
        seal = read_member(candidate, 'candidate.sha256', 65).decode().strip()
        head = self._head(job, seal)
        history = area/'attestation-history'
        committed = []
        if history.exists():
            self._safe(history)
            names = []
            with os.scandir(history) as listing:
                for item in listing:
                    require(len(names) < MAX_ATTESTATIONS, 'attestation.changed', 'Retained history exceeds cap.')
                    names.append(item.name)
            names.sort()
            require(names == [f'{i:04d}.json' for i in range(len(names))]
                    and len(names) <= MAX_ATTESTATIONS, 'attestation.changed', 'Malformed retained history.')
            committed = [_json(read_member(area, 'attestation-history/'+name, MAX_JSON)) for name in names]
        if head['pending'] is not None and state['pending'] is None and (not state['entries'] or state['entries'][-1] != head['pending']):
            state['pending'] = head['pending']
        require(len(committed) == len(state['entries']) or state['pending'] is not None
                and len(committed) == len(state['entries'])+1, 'attestation.changed', 'Anchor differs from committed history.')
        # Validate the independent committed count/head, including the one
        # prepared transaction that recovery can finish in either write order.
        pending = head['pending']
        require(state['pending'] is None or state['pending'] == pending,
                'attestation.changed', 'Prepared append differs from authoritative state.')
        require(all(isinstance(e, dict) and set(e) == {'name', 'sha256', 'previous', 'payload'} for e in state['entries']),
                'attestation.changed', 'Malformed committed entries.')
        pointer_entries = state['entries']
        if head['count'] == len(pointer_entries) + 1 and pending is not None:
            pointer_entries = state['entries'] + [pending]
        require(head['count'] == len(pointer_entries)
                and head['head'] == (pointer_entries[-1]['sha256'] if pointer_entries else '0'*64),
                'attestation.changed', 'Committed attestation history was truncated or replaced.')
        if pending is not None:
            require(isinstance(pending, dict) and (state['pending'] == pending
                    or state['entries'] and state['entries'][-1] == pending),
                    'attestation.changed', 'Authoritative prepared append is missing.')
        previous, expected, result, hashes = '0'*64, set(), [], set()
        entries = state['entries'] + ([state['pending']] if state['pending'] else [])
        for index, entry in enumerate(entries):
            require(isinstance(entry, dict) and set(entry) == {'name', 'sha256', 'previous', 'payload'}
                    and entry['previous'] == previous and isinstance(entry['payload'], dict)
                    and entry['payload'].get('previous') == previous
                    and entry['payload'].get('candidate_sha256') == seal
                    and entry['payload'].get('candidate_id') == job
                    and isinstance(entry['name'], str) and entry['name'] not in expected
                    and isinstance(entry['sha256'], str) and entry['sha256'] not in hashes
                    and isinstance(entry['payload'].get('kind'), str) and isinstance(entry['payload'].get('utc'), str)
                    and entry['name'] == entry['payload']['kind']+'.'+entry['payload']['utc']+'.json'
                    and re.fullmatch(r'[a-z][a-z0-9_]{0,31}\.\d{8}T\d{12}Z\.json', entry['name']),
                    'attestation.changed', 'Malformed attestation chain.')
            if index < len(committed):
                require(entry == committed[index], 'attestation.changed', 'Entry differs from retained committed history.')
            data = canonical(entry['payload'])+b'\n'
            require(digest(data) == entry['sha256'], 'attestation.changed', 'Anchor payload changed.')
            name = entry['name']; expected |= {name, name+'.sha256'}
            for member, wanted in ((name, data), (name+'.sha256', (entry['sha256']+'\n').encode())):
                path = candidate/'attestations'/member
                if path.exists():
                    require(read_member(candidate, 'attestations/'+member, MAX_JSON) == wanted, 'attestation.changed', 'Attestation rewritten.')
                else:
                    require(index == len(state['entries']), 'attestation.changed', 'Attestation truncated.')
            previous = entry['sha256']; hashes.add(previous)
            if index < len(state['entries']): result.append(entry['payload'])
        if (candidate/'attestations').exists():
            self._safe(candidate/'attestations')
            with os.scandir(candidate/'attestations') as listing:
                actual = set()
                for entry in listing:
                    require(len(actual) < MAX_ATTESTATIONS*2+2 and not path_is_link(Path(entry.path), entry.stat(follow_symlinks=False)),
                            'attestation.changed', 'Unsafe attestation directory.')
                    actual.add(entry.name)
            require(actual <= expected, 'attestation.changed', 'Unregistered attestations.')
        return result

    def append_attestation(self, job, kind, utc, payload):
        require(isinstance(kind, str) and re.fullmatch(r'[a-z][a-z0-9_]{0,31}', kind)
                and isinstance(utc, str) and re.fullmatch(r'\d{8}T\d{12}Z', utc), 'attestation.invalid', 'Use a kind and UTC microsecond timestamp.')
        self._reject(kind, utc, payload)
        data = canonical(payload)+b'\n'
        require(len(data) <= MAX_JSON, 'candidate.limit', 'Attestation exceeds byte cap.')
        _json(data)
        with self._guard():
            bundle = self.read(job)
            state = self._attestation_state(job)
            require(not state['pending'] and self._head(job, bundle['sha256'])['pending'] is None,
                    'attestation.pending', 'Recover the pending append first.')
            require(len(state['entries']) < MAX_ATTESTATIONS, 'candidate.limit', 'Attestation quota exceeded.')
            name = kind+'.'+utc+'.json'
            require(all(e['name'] != name for e in state['entries']), 'attestation.changed', 'Attestations cannot be rewritten.')
            previous = state['entries'][-1]['sha256'] if state['entries'] else '0'*64
            payload = {'candidate_sha256': bundle['sha256'], 'candidate_id': job, 'previous': previous,
                       'kind': kind, 'utc': utc, 'observation': payload}
            data = canonical(payload)+b'\n'
            entry = {'name': name, 'sha256': digest(data), 'previous': state['entries'][-1]['sha256'] if state['entries'] else '0'*64, 'payload': payload}
            state['pending'] = entry
            _, area = self.paths(job)
            head = self._head(job, bundle['sha256']); head['pending'] = entry
            committed = {'schema': state['schema'], 'entries': state['entries']+[entry], 'pending': None}
            advanced_head = {**head, 'count': len(committed['entries']), 'head': entry['sha256']}
            # Each prospective record combines at most the bounded anchor and
            # one bounded payload. Check every reader's byte/depth/item caps
            # before the first durable transaction pointer is installed.
            prepared_bytes = _record_bytes(state)
            for record in (payload, entry, committed, head, advanced_head, {**advanced_head, 'pending': None}):
                _record_bytes(record)
            config = bundle['provenance']['config']
            effective = verify_effective_config(bundle['members']['effective_config.json'],
                config['effective_config_sha256'], config['config_digest'])
            self._quota(len(prepared_bytes)+2*len(data)+1024, effective['retention']['max_total_mib']*1024**2)
            self._write_head(job, head)
            self._write(area/'attestations.json', prepared_bytes, state)
            return self._recover_attestation(job)

    def _recover_attestation(self, job):
        self.attestations(job)
        state = self._attestation_state(job)
        candidate, area = self.paths(job)
        seal = read_member(candidate, 'candidate.sha256', 65).decode().strip()
        head = self._head(job, seal)
        if not state['pending']:
            if head['pending'] is None: return self.attestations(job)
            if state['entries'] and state['entries'][-1] == head['pending']:
                head['pending'] = None; self._write_head(job, head)
                return self.attestations(job)
            state['pending'] = head['pending']
        entry = state['pending']
        for name, data in ((entry['name'], canonical(entry['payload'])+b'\n'), (entry['name']+'.sha256', (entry['sha256']+'\n').encode())):
            if not (candidate/'attestations'/name).exists(): self._write(candidate/'attestations'/name, data, entry['payload'])
        history_name = f"attestation-history/{len(state['entries']):04d}.json"
        history_data = canonical(entry)+b'\n'
        if (area/history_name).exists():
            require(read_member(area, history_name, MAX_JSON) == history_data, 'attestation.changed', 'Committed history changed.')
        else: self._write(area/history_name, history_data, entry)
        head.update(count=len(state['entries'])+1, head=entry['sha256'])
        self._write_head(job, head)
        state['entries'].append(entry); state['pending'] = None
        self._write(area/'attestations.json', canonical(state)+b'\n', state)
        head['pending'] = None
        self._write_head(job, head)
        return self.attestations(job)

    def recover_attestation(self, job):
        with self._guard(): return self._recover_attestation(job)

    def delete(self, job):
        self.paths(job)
        raise Refusal('candidate.retained', 'Manual GC is unavailable; retained evidence cannot be deleted.')


from .meshgen_jobs import RequestLedger


class CandidateLedger(RequestLedger):
    """E4 opt-in ledger: a committed seal wins cancellation and orphan recovery.

    E3's RequestLedger remains unchanged. G2 owners use this ledger and invoke
    commit_success only after backend and worker tasks are stopped and drained.
    Lock order is owner transition -> candidate-storage guard -> ledger guard.
    """
    def _store(self, document=None):
        return CandidateStore(self.project, document or self.document, tokens=self.secret_source())

    def _sealed(self, identifier, document=None):
        store = self._store(document)
        candidate, _ = store.paths(identifier)
        store._safe(candidate)
        return (candidate/'candidate.sha256').exists()

    def finish(self, identifier, state, *, result=None, errors=()):
        with self.mutex:
            if self._sealed(identifier):
                record = self.status(identifier)
                require(not record.get('backend_job_id') or record.get('backend_stopped') is True and record.get('task_drained') is True,
                        'busy', 'Committed candidate still has unreconciled owned work.')
                result = self._store().recover(identifier)
                require(result['request_id'] == record['request_id'] and result['plan_digest'] == record['plan_digest'],
                        'receipt.changed', 'Candidate receipt differs from owner plan.')
                state, errors = 'succeeded', ()
            return super().finish(identifier, state, result=result, errors=errors)

    def _recover_record(self, record, document):
        if not self._sealed(record['job_id'], document): return super()._recover_record(record, document)
        require(not record.get('backend_job_id') or record.get('backend_stopped') is True and record.get('task_drained') is True,
                'busy', 'Committed candidate has unconfirmed backend ownership.')
        receipt = self._store(document).recover(record['job_id'])
        require(receipt['request_id'] == record['request_id'] and receipt['plan_digest'] == record['plan_digest'],
                'receipt.changed', 'Recovered candidate differs from owner plan.')
        record.update(state='succeeded', result=receipt, errors=[], backend_stopped=True, task_drained=True)
        self._publish(record, document)
        return True
