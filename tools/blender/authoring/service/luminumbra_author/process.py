"""Own the compiler until it exits, times out, or its service pipe closes."""
from __future__ import annotations

import os
from pathlib import Path
import signal
import subprocess
import sys
import threading

from .contracts import atomic_json

LOG_LIMIT = 64 * 1024


def supervise(command, result_path, timeout):
    """Internal child entry point. EOF on stdin also handles service crashes."""
    log = bytearray()
    cancelled = threading.Event()
    timed_out = False
    process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, start_new_session=os.name != "nt")

    def stop():
        if process.poll() is None:
            try:
                if os.name == "nt":
                    # The registered asset_processor is a single native process.
                    process.kill()
                else:
                    os.killpg(process.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass

    def watch_parent():
        # Parent sends no payload: retaining this pipe is the ownership signal.
        while os.read(sys.stdin.fileno(), 1):
            pass
        cancelled.set()
        stop()

    total = 0

    def drain():
        nonlocal total
        for block in iter(lambda: process.stdout.read(8192), b""):
            total += len(block)
            log.extend(block[:max(0, LOG_LIMIT - len(log))])

    watcher = threading.Thread(target=watch_parent, daemon=True)
    reader = threading.Thread(target=drain, daemon=True)
    watcher.start()
    reader.start()
    try:
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            stop()
            process.wait()
    finally:
        stop()
        reader.join(timeout=5)
        process.stdout.close()
    atomic_json(Path(result_path), {
        "returncode": process.returncode, "cancelled": cancelled.is_set(), "timed_out": timed_out,
        "log": log.decode("utf-8", errors="replace"), "log_truncated": total > LOG_LIMIT,
        "log_bytes": total,
    })
