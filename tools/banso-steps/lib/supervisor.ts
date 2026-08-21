import { spawn, type ChildProcess } from "node:child_process";
import { performance } from "node:perf_hooks";
import {
  DEFAULT_CAPTURE_POLICY,
  type CaptureMode,
  type CapturePolicy,
  type CapturedStream,
  type ProducedArtifact,
  type ProcessResult,
  type StreamCapturePolicy,
  normalizeProcessResult,
} from "./result.ts";

export interface CommandSpec {
  executable: string;
  args: string[];
}

export interface PartialStreamCapturePolicy {
  mode?: CaptureMode;
  maxBytes?: number;
}

export interface PartialCapturePolicy {
  stdout?: PartialStreamCapturePolicy;
  stderr?: PartialStreamCapturePolicy;
}

export interface RunProcessOptions {
  executable: string;
  args?: readonly string[];
  cwd?: string;
  env?: NodeJS.ProcessEnv;
  timeoutMs?: number;
  signal?: AbortSignal;
  capture?: PartialCapturePolicy;
  artifacts?: readonly ProducedArtifact[];
  killGraceMs?: number;
}

interface CaptureCollector {
  result(): CapturedStream;
  write(chunk: Buffer | string): void;
}

function assertArgument(value: string, label: string): void {
  if (typeof value !== "string") {
    throw new TypeError(`${label} must be a string`);
  }
  if (value.includes("\0")) {
    throw new TypeError(`${label} must not contain a null byte`);
  }
}

export function buildArgv(executable: string, args: readonly string[] = []): string[] {
  assertArgument(executable, "executable");
  if (executable.length === 0) {
    throw new TypeError("executable must not be empty");
  }
  args.forEach((arg, index) => assertArgument(arg, `args[${index}]`));
  return [executable, ...args];
}

export function createCommand(executable: string, args: readonly string[] = []): CommandSpec {
  const argv = buildArgv(executable, args);
  return { executable: argv[0], args: argv.slice(1) };
}

function normalizeStreamPolicy(
  partial: PartialStreamCapturePolicy | undefined,
  fallback: StreamCapturePolicy,
): StreamCapturePolicy {
  const mode = partial?.mode ?? fallback.mode;
  const maxBytes = partial?.maxBytes ?? fallback.maxBytes;
  if (!Number.isSafeInteger(maxBytes) || maxBytes < 0) {
    throw new RangeError("capture maxBytes must be a non-negative safe integer");
  }
  return { mode, maxBytes };
}

export function normalizeCapturePolicy(partial: PartialCapturePolicy = {}): CapturePolicy {
  return {
    stdout: normalizeStreamPolicy(partial.stdout, DEFAULT_CAPTURE_POLICY.stdout),
    stderr: normalizeStreamPolicy(partial.stderr, DEFAULT_CAPTURE_POLICY.stderr),
  };
}

function createCollector(policy: StreamCapturePolicy): CaptureCollector {
  const chunks: Buffer[] = [];
  let storedBytes = 0;
  let observedBytes = 0;

  return {
    write(chunk): void {
      const buffer = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk);
      observedBytes += buffer.byteLength;
      const remaining = policy.maxBytes - storedBytes;
      if (policy.mode === "capture" && remaining > 0) {
        const stored = buffer.subarray(0, remaining);
        chunks.push(stored);
        storedBytes += stored.byteLength;
      }
    },
    result(): CapturedStream {
      return {
        policy: { ...policy },
        text: policy.mode === "capture" ? Buffer.concat(chunks, storedBytes).toString("utf8") : null,
        bytes: observedBytes,
        truncated: policy.mode === "capture" && observedBytes > storedBytes,
      };
    },
  };
}

function stdioFor(mode: CaptureMode): "pipe" | "inherit" | "ignore" {
  if (mode === "capture") return "pipe";
  if (mode === "inherit") return "inherit";
  return "ignore";
}

function validateMilliseconds(value: number | undefined, label: string): void {
  if (value === undefined) return;
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new RangeError(`${label} must be a non-negative safe integer`);
  }
}

function terminate(child: ChildProcess, force: boolean): void {
  if (child.exitCode !== null || child.signalCode !== null) return;

  const signal: NodeJS.Signals = force ? "SIGKILL" : "SIGTERM";
  try {
    child.kill(signal);
  } catch {
    // A concurrent process exit needs no further action.
  }
}

export async function runProcess(options: RunProcessOptions): Promise<ProcessResult> {
  const command = createCommand(options.executable, options.args);
  validateMilliseconds(options.timeoutMs, "timeoutMs");
  validateMilliseconds(options.killGraceMs, "killGraceMs");

  const capture = normalizeCapturePolicy(options.capture);
  const stdout = createCollector(capture.stdout);
  const stderr = createCollector(capture.stderr);
  const started = performance.now();

  if (options.signal?.aborted) {
    return normalizeProcessResult({
      durationMs: performance.now() - started,
      cancelled: true,
      stdout: stdout.result(),
      stderr: stderr.result(),
      artifacts: options.artifacts,
    });
  }

  return await new Promise<ProcessResult>((resolve) => {
    let timedOut = false;
    let cancelled = false;
    let settled = false;
    let timeoutTimer: NodeJS.Timeout | undefined;
    let forceTimer: NodeJS.Timeout | undefined;
    let spawnError: Error | null = null;

    const child = spawn(command.executable, command.args, {
      cwd: options.cwd,
      env: options.env,
      shell: false,
      windowsHide: true,
      stdio: ["ignore", stdioFor(capture.stdout.mode), stdioFor(capture.stderr.mode)],
    });

    child.stdout?.on("data", (chunk: Buffer) => stdout.write(chunk));
    child.stderr?.on("data", (chunk: Buffer) => stderr.write(chunk));

    const requestStop = (reason: "timeout" | "cancelled"): void => {
      if (settled || timedOut || cancelled) return;
      timedOut = reason === "timeout";
      cancelled = reason === "cancelled";
      terminate(child, false);
      forceTimer = setTimeout(
        () => terminate(child, true),
        options.killGraceMs ?? 250,
      );
      forceTimer.unref();
    };

    const onAbort = (): void => requestStop("cancelled");
    options.signal?.addEventListener("abort", onAbort, { once: true });

    if (options.timeoutMs !== undefined) {
      timeoutTimer = setTimeout(() => requestStop("timeout"), options.timeoutMs);
      timeoutTimer.unref();
    }

    const finish = (exitCode: number | null, signal: NodeJS.Signals | null): void => {
      if (settled) return;
      settled = true;
      if (timeoutTimer) clearTimeout(timeoutTimer);
      if (forceTimer) clearTimeout(forceTimer);
      options.signal?.removeEventListener("abort", onAbort);
      resolve(normalizeProcessResult({
        exitCode,
        signal,
        durationMs: performance.now() - started,
        timedOut,
        cancelled,
        spawnError,
        stdout: stdout.result(),
        stderr: stderr.result(),
        artifacts: options.artifacts,
      }));
    };

    child.once("error", (error) => {
      spawnError = error;
      finish(null, null);
    });
    child.once("close", finish);
  });
}

export const supervise = runProcess;
