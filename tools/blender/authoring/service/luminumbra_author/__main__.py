"""Installed CLI and authenticated, inherited-pipe RPC interface."""
import argparse
import hmac
import os
from pathlib import Path
import sys
import time

from .contracts import ASSET, Refusal, canonical, keys, parse_json, require
from .service import BuildService, TERMINAL


def dispatch(service, operation, params):
    if operation in ("capabilities", "registry"):
        keys(params, ())
        return service.capabilities() if operation == "capabilities" else {
            "schemas": [ASSET], "components": [], "nodes": []}
    if operation == "validate":
        keys(params, ("source",))
        return service.validate(params["source"])
    if operation == "build":
        keys(params, ("source", "revision"))
        return service.submit(params["source"], params["revision"])
    if operation in ("job.status", "job.cancel"):
        keys(params, ("job_id",))
        method = service.status if operation == "job.status" else service.cancel
        return method(params["job_id"])
    if operation == "generation.inspect":
        keys(params, (), ("job_id",))
        return service.generation(params.get("job_id"))
    raise Refusal("operation.unsupported", f"Operation {operation!r} is unavailable.")


def serve(service):
    token = os.environ.pop("LUMINUMBRA_AUTHOR_TOKEN", "")
    require(len(token) >= 32, "session.token", "Set an unpredictable session token of at least 32 characters.")
    while True:
        line = sys.stdin.buffer.readline(65537)
        if not line:
            break
        request_id = None
        try:
            require(len(line) <= 65536, "request.limit", "Request exceeds 64 KiB.")
            request = parse_json(line)
            keys(request, ("id", "token", "op", "params"))
            require(type(request["id"]) is int and request["id"] >= 0,
                    "request.id", "Request id must be a nonnegative integer.")
            request_id = request["id"]
            require(isinstance(request["token"], str) and hmac.compare_digest(request["token"], token),
                    "session.unauthorized", "Invalid session token.")
            result = {"id": request_id, "ok": True,
                      "result": dispatch(service, request["op"], request["params"])}
        except (Refusal, ValueError, TypeError, KeyError, OSError, RecursionError) as error:
            refusal = error if isinstance(error, Refusal) else Refusal("request.invalid", str(error))
            result = {"id": request_id, "ok": False, "findings": [refusal.finding]}
        print(canonical(result).decode(), flush=True)
        if len(line) > 65536:
            break


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "_compiler":
        from .process import supervise
        supervise(sys.argv[4:], sys.argv[2], float(sys.argv[3]))
        return 0
    parser = argparse.ArgumentParser(description="Compile GLB geometry into validated retained generations.")
    parser.add_argument("--project", type=Path, required=True)
    parser.add_argument("--toolchain", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=120)
    sub = parser.add_subparsers(dest="operation", required=True)
    for operation in ("capabilities", "registry", "serve"):
        sub.add_parser(operation)
    for operation in ("validate", "build"):
        command = sub.add_parser(operation)
        command.add_argument("source")
        if operation == "build":
            command.add_argument("--revision", type=int, required=True)
    for operation in ("job.status", "job.cancel", "generation.inspect"):
        command = sub.add_parser(operation)
        command.add_argument("--job-id", required=operation != "generation.inspect")
    args = parser.parse_args()
    try:
        with BuildService(args.project, args.toolchain, timeout=args.timeout) as service:
            if args.operation == "serve":
                serve(service)
                return 0
            params = {key: getattr(args, key) for key in ("source", "revision", "job_id")
                      if getattr(args, key, None) is not None}
            result = dispatch(service, args.operation, params)
            if args.operation == "build":
                while result["status"] not in TERMINAL:
                    time.sleep(0.02)
                    result = service.status(result["job_id"])
            print(canonical({"ok": True, "result": result}).decode())
            return int(args.operation == "build" and result["status"] != "succeeded")
    except (Refusal, ValueError, TypeError, KeyError, OSError, RecursionError) as error:
        refusal = error if isinstance(error, Refusal) else Refusal("request.invalid", str(error))
        print(canonical({"ok": False, "findings": [refusal.finding]}).decode())
        return 1
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    raise SystemExit(main())
