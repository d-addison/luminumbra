"""Pinned finisher envelope and process-owned simulated worker; no Blender launch.

Portable simulation exercises the same bounded pipe/tree ownership mechanics.
The native lane is unavailable until a qualified installed game finisher exists.
"""
import os
from pathlib import Path
import sys
import threading
import time

from .bootstrap import isolated_path
from .contracts import Refusal, canonical, digest, keys, require, read_bounded
from .meshgen_backend import bounded_json
from .meshgen_config import verify_effective_config
from .meshgen_jobs import reject_secret
from .process import run_owned

MAX_REQUEST = 65536
BOOTSTRAP = 'import sys; sys.path[:]=__import__("json").loads(sys.argv.pop(1)); from luminumbra_author.meshgen_worker import main; raise SystemExit(main())'


def command(mode):
    return [sys.executable, '-I', '-S', '-B', '-c', BOOTSTRAP,
            canonical(isolated_path(str(Path(__file__).resolve().parent.parent))).decode(), mode]


def verify_game_scripts(toolchain, project):
    """Static pins now; run_pinned_script rechecks and guards imports at execution."""
    return toolchain.verify_scripts(project, 'finisher')


def finisher_envelope(toolchain, project, resolved, input_path, output_dir):
    """Reviewable native contract. Does not claim native enforcement qualification."""
    pinned = verify_game_scripts(toolchain, project)
    effective = resolved.effective
    require(effective['finish']['decimate']['method'] == 'blender_collapse'
            and effective['finish']['uv']['method'] in ('smart_project', 'none'),
            'worker.unavailable', 'The requested native helper is unavailable.')
    require(effective['workers']['finisher']['cpu_threads'] <= 2, 'worker.limit', 'Finisher thread cap exceeded.')
    project = Path(project).absolute()
    output = Path(output_dir).absolute()
    raw = Path(input_path).absolute()
    require(all('..' not in path.parts for path in (project, raw, output)),
            'candidate.path', 'Worker traversal is unsupported.')
    from . import file_io
    try:
        file_io.safe_path(project); file_io.safe_path(raw); file_io.safe_path(output)
    except ValueError as error:
        raise Refusal('candidate.path', 'Worker paths cannot use reparse points.') from error
    # Normalize existing DOS aliases only after traversal/reparse refusal;
    # scratch leaves need not exist yet.
    project, raw, output = project.resolve(), raw.resolve(), output.resolve()
    require(raw.is_relative_to(project/'.luminumbra-author-recipes')
            and output.is_relative_to(project/'.luminumbra-author-recipes'),
            'candidate.path', 'Worker paths must remain in project state.')
    request = {'effective_config_path': str(output/'effective_config.json'),
               'effective_config_sha256': resolved.stored_sha256, 'config_digest': resolved.config_digest,
               'input_glb': str(raw), 'output_dir': str(output),
               'script_sha256': pinned['sha256']}
    payload = canonical(request)+b'\n'
    require(len(payload) <= MAX_REQUEST, 'worker.limit', 'Worker request exceeds bound.')
    return {'script': pinned, 'blender_args': ['--background', '--factory-startup', '--disable-autoexec',
            '--threads', str(effective['workers']['finisher']['cpu_threads'])],
            'stdin': payload, 'timeout': effective['timeouts']['finish_s'],
            'memory_mib': effective['workers']['finisher']['memory_mib'],
            'resource_environment': {k: str(output/k.lower()) for k in ('BLENDER_USER_CONFIG', 'BLENDER_USER_SCRIPTS', 'BLENDER_USER_DATAFILES')},
            'qualified': False}


def run_native(*args, **kwargs):
    raise Refusal('worker.unavailable', 'Native Blender finisher acceptance is pending; use the simulated portable lane.')


def run_simulated(config_path, stored_sha256, config_digest, *, mode='normal', cancel=None,
                  readiness_timeout=5, clock=time.monotonic, token=None):
    data = read_bounded(config_path, 1024**2)
    effective = verify_effective_config(data, stored_sha256, config_digest)
    require(mode in ('normal', 'no_ready', 'hang', 'crash', 'oversize', 'policy', 'descendant'),
            'worker.protocol', 'Unknown simulated mode.')
    request = {'config_path': str(Path(config_path).absolute()), 'stored_sha256': stored_sha256,
               'config_digest': config_digest, 'mode': mode, 'timeout': effective['timeouts']['finish_s'],
               'memory_mib': effective['workers']['finisher']['memory_mib']}
    reject_secret(token, request, data, effective)
    # Only a small OS baseline; project/backend/session credentials never enter the child environment.
    env = {k: v for k, v in os.environ.items() if k.upper() in ('SYSTEMROOT', 'WINDIR', 'PATH', 'TMPDIR', 'TMP', 'TEMP')}
    result = run_owned(command('_supervisor'), canonical(request)+b'\n', timeout=request['timeout'],
                       readiness_timeout=readiness_timeout, cancel=cancel, clock=clock, environment=env,
                       memory_mib=request['memory_mib'], retain_stdin=True)
    observation = bounded_json(result['stdout'])
    reject_secret(token, observation, result['log'])
    keys(observation, ('simulated', 'config_digest', 'effective_config_sha256', 'observations'))
    require(observation['simulated'] is True and observation['config_digest'] == config_digest
            and observation['effective_config_sha256'] == stored_sha256, 'worker.protocol', 'Worker configuration identity differs.')
    from .meshgen_provenance import validate_observations
    validate_observations(observation['observations'], effective)
    return observation


def _request():
    data = sys.stdin.buffer.readline(MAX_REQUEST+1)
    require(len(data) <= MAX_REQUEST and data.endswith(b'\n'), 'worker.limit', 'Invalid bounded worker request.')
    request = bounded_json(data, max_bytes=MAX_REQUEST)
    keys(request, ('config_path', 'stored_sha256', 'config_digest', 'mode', 'timeout', 'memory_mib'))
    return request


def _supervisor():
    if os.name == 'posix':
        from .process import _enable_subreaper
        _enable_subreaper()
    request = _request()  # Windows Job assignment precedes this handshake.
    effective = verify_effective_config(read_bounded(request['config_path'], 1024**2), request['stored_sha256'], request['config_digest'])
    require(type(request['timeout']) in (int, float) and request['timeout'] == effective['timeouts']['finish_s']
            and type(request['memory_mib']) is int and request['memory_mib'] == effective['workers']['finisher']['memory_mib'],
            'worker.protocol', 'Worker resource limits differ from resolved configuration.')
    if os.name == 'posix':
        import resource
        cap = request['memory_mib']*1024**2
        resource.setrlimit(resource.RLIMIT_AS, (cap, cap))
    cancelled = threading.Event()
    def watch_owner():
        while os.read(sys.stdin.fileno(), 1): pass
        cancelled.set()
    threading.Thread(target=watch_owner, daemon=True).start()
    # Propagate readiness only after the owned child verifies its stored config.
    result = run_owned(command('_simulator'), canonical(request)+b'\n', timeout=request['timeout'],
                       readiness_timeout=min(5, request['timeout']), cancel=cancelled, ready_callback=_ready,
                       memory_mib=request['memory_mib'], own_tree=os.name == 'nt')
    sys.stdout.buffer.write(result['stdout']); sys.stdout.buffer.flush()
    sys.stderr.buffer.write(result['log']); sys.stderr.buffer.flush()
    return 0


def _simulator():
    request = _request()
    effective = verify_effective_config(read_bounded(request['config_path'], 1024**2), request['stored_sha256'], request['config_digest'])
    mode = request['mode']
    if mode == 'no_ready':
        threading.Event().wait(request['timeout']+1); return 0
    _ready()
    if mode == 'crash': return 7
    if mode in ('hang', 'descendant'):
        if mode == 'descendant':
            import subprocess
            child = subprocess.Popen([sys.executable, '-I', '-S', '-B', '-c', 'import threading; threading.Event().wait(3600)'],
                                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                     start_new_session=os.name != 'nt')
            from .meshgen_jobs import durable_write
            durable_write(Path(request['config_path']).with_suffix('.descendant.pid'), str(child.pid).encode())
            durable_write(Path(request['config_path']).with_suffix('.worker.pids'), canonical([os.getpid(), child.pid]))
        threading.Event().wait(request['timeout']+1); return 0
    if mode == 'oversize':
        sys.stdout.buffer.write(b'x'*(1024**2+1)); sys.stdout.buffer.flush(); return 0
    exporter = digest(b'explicitly simulated exporter')
    observations = {'finish': {'blender_version': 'simulated', 'exporter_sha256': exporter,
        'steps': [{'id': step, 'params': {}, 'outcome': 'simulated', 'stats': {}} for step in effective['finish']['steps']],
        'tool_versions': {'preview': 'simulated-fixture-v1'}},
        'guard': {'hit_rate_before': 1, 'hit_rate_after': 1, 'p10_before': 1, 'p10_after': 1, 'components_survived': True}}
    if mode == 'policy': observations['flags'] = {'shippable': True}
    print(canonical({'simulated': True, 'config_digest': request['config_digest'],
          'effective_config_sha256': request['stored_sha256'], 'observations': observations}).decode(), flush=True)
    return 0


def _ready():
    sys.stdout.buffer.write(b'READY\n'); sys.stdout.buffer.flush()


def main():
    try:
        return {'_supervisor': _supervisor, '_simulator': _simulator}[sys.argv[1]]()
    except (Refusal, ValueError, OSError, KeyError) as error:
        finding = error.finding if isinstance(error, Refusal) else Refusal('worker.failed', str(error)).finding
        print(canonical({'ok': False, 'findings': [finding]}).decode(), file=sys.stderr, flush=True)
        return 1


if __name__ == '__main__': raise SystemExit(main())
