"""Build pinned asset/animation tooling with private source, dependencies and caches."""
import argparse
import json
from datetime import datetime, timezone
from pathlib import Path
import subprocess

from common import ROOT, SOURCE, PIN, file_digest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dependency-source', type=Path, required=True,
                        help='Read-only CMake dependency cache containing meshoptimizer-src, stb-src and entt-src Git clones')
    args = parser.parse_args()
    (ROOT / 'evidence').mkdir(exist_ok=True)
    receipt = {'scope': 'CPU asset/animation evidence build only; no release build tree', 'commands': []}
    output = ROOT / 'evidence' / ('tool-build-' + datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ') + '.json')
    def run(command):
        result = subprocess.run([str(x) for x in command], cwd=ROOT, text=True, capture_output=True, timeout=180)
        receipt['commands'].append({'argv': [str(x) for x in command], 'returncode': result.returncode,
                                    'stdout': result.stdout, 'stderr': result.stderr})
        output.write_text(json.dumps(receipt, indent=2))
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        return result.stdout.strip()
    if run(['git', '-C', SOURCE, 'rev-parse', 'HEAD']) != PIN or run(['git', '-C', SOURCE, 'status', '--porcelain']):
        raise RuntimeError('Select a clean independent baseline clone with LUMINUMBRA_AUTHOR_SOURCE')
    receipt['source_commit'] = PIN
    deps = ROOT / 'dependencies'
    deps.mkdir(exist_ok=True)
    receipt['dependencies'] = {}
    for name, commit in [('meshoptimizer', '4affad044571506a5724c9a6f15424f43e86f731'),
                         ('stb', '31c1ad37456438565541f4919958214b6e762fb4'),
                         ('entt', 'd4014c74dc3793aba95ae354d6e23a026c2796db')]:
        target = deps / name
        source = args.dependency_source / (name + '-src')
        if not target.exists():
            run(['git', 'clone', '--no-hardlinks', '--no-checkout', source, target])
            run(['git', '-C', target, 'checkout', '--detach', commit])
        if run(['git', '-C', target, 'rev-parse', 'HEAD']) != commit or run(['git', '-C', target, 'status', '--porcelain']):
            raise RuntimeError('Dependency identity/cleanliness mismatch: ' + name)
        receipt['dependencies'][name] = commit
    build = ROOT / 'build' / 'asset-evidence'
    install = ROOT / 'install' / 'asset-evidence'
    run(['cmake', '-S', ROOT / 'probes/tool_build', '-B', build, '-G', 'Ninja',
         '-DPROBE_ROOT=' + str(ROOT), '-DENGINE_SOURCE=' + str(SOURCE), '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_INSTALL_PREFIX=' + str(install)])
    run(['cmake', '--build', build, '--parallel', '2'])
    run(['cmake', '--install', build])
    receipt['binaries'] = {str(path.relative_to(ROOT)): file_digest(path) for path in (install / 'bin').iterdir()}
    receipt['source_commit'] = PIN
    receipt['passed'] = True
    output.write_text(json.dumps(receipt, indent=2))
    print(json.dumps({'passed': True, 'receipt': str(output), 'binaries': receipt['binaries']}, indent=2))


if __name__ == '__main__':
    main()
