#!/usr/bin/env python3
"""Run the real production-pass tests on an owned Xvfb + forced llvmpipe only."""
import argparse
import os
from pathlib import Path
import selectors
import signal
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--binary', required=True)
    parser.add_argument('--xvfb', default='/usr/bin/Xvfb')
    parser.add_argument('--output')
    args = parser.parse_args()
    output = Path(args.output) if args.output else Path(tempfile.mkdtemp(prefix='luminumbra-static-software-'))
    if args.output:
        output.mkdir(parents=False, exist_ok=False)
    env = dict(os.environ)
    env.update(LIBGL_ALWAYS_SOFTWARE='1', GALLIUM_DRIVER='llvmpipe',
               MESA_LOADER_DRIVER_OVERRIDE='swrast', __GLX_VENDOR_LIBRARY_NAME='mesa',
               LP_NUM_THREADS='2', MESA_GLTHREAD='false')
    read_fd, write_fd = os.pipe()
    server = None
    try:
        with (output / 'xvfb.log').open('wb') as log:
            # Linux's abstract local socket needs no write to /tmp/.X11-unix
            # (a read-only WSLg mount on WSL). TCP remains disabled.
            transport = ['-nolisten', 'unix', '-listen', 'local'] if sys.platform == 'linux' else []
            server = subprocess.Popen([args.xvfb, '-displayfd', str(write_fd), '-screen', '0',
                                       '1280x720x24', '-nolisten', 'tcp', *transport],
                                      pass_fds=(write_fd,), stdout=log, stderr=log,
                                      start_new_session=True, env=env)
            os.close(write_fd)
            write_fd = -1
            selector = selectors.DefaultSelector()
            selector.register(read_fd, selectors.EVENT_READ)
            if not selector.select(10):
                raise RuntimeError('Owned software display did not become ready')
            display = os.read(read_fd, 32).decode('ascii').strip()
            selector.close()
            if not display.isdigit():
                raise RuntimeError('Invalid owned software display identity')
            env['DISPLAY'] = ':' + display
            result = subprocess.run([args.binary, '--gtest_output=xml:' + str(output / 'tests.xml')],
                                    env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                    timeout=180, check=False)
            (output / 'tests.log').write_bytes(result.stdout)
            print(result.stdout.decode('utf-8', errors='replace'), end='')
            if result.returncode:
                raise RuntimeError(f'Production software rendering tests exited {result.returncode}')
            root = ET.parse(output / 'tests.xml').getroot()
            cases = root.findall('.//testcase')
            if len(cases) != 11 or len({(x.get('classname'), x.get('name')) for x in cases}) != 11:
                raise RuntimeError('Expected exactly eleven unique production rendering tests')
            if any(x.find('skipped') is not None or x.find('failure') is not None or
                   x.get('status') != 'run' for x in cases):
                raise RuntimeError('Rendering tests contain a skipped, failed or unexecuted case')
            print(f'Software GL: 11 actual llvmpipe cases passed; artifacts: {output}')
    finally:
        if write_fd >= 0:
            os.close(write_fd)
        os.close(read_fd)
        if server is not None and server.poll() is None:
            os.killpg(server.pid, signal.SIGTERM)
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                os.killpg(server.pid, signal.SIGKILL)
                server.wait(timeout=5)


if __name__ == '__main__':
    main()
