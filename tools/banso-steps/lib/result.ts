export type ExitClass =
  | "success"
  | "failed"
  | "timed_out"
  | "cancelled"
  | "signalled"
  | "spawn_error";

export type CaptureMode = "capture" | "inherit" | "discard";

export interface StreamCapturePolicy {
  mode: CaptureMode;
  maxBytes: number;
}

export interface CapturePolicy {
  stdout: StreamCapturePolicy;
  stderr: StreamCapturePolicy;
}

export interface CapturedStream {
  policy: StreamCapturePolicy;
  text: string | null;
  bytes: number;
  truncated: boolean;
}

export interface ProducedArtifact {
  name: string;
  path: string;
  kind?: string;
  bytes?: number;
  sha256?: string;
}

export interface ProcessResult {
  exitClass: ExitClass;
  exitCode: number | null;
  signal: NodeJS.Signals | null;
  durationMs: number;
  timedOut: boolean;
  cancelled: boolean;
  stdout: CapturedStream;
  stderr: CapturedStream;
  artifacts: ProducedArtifact[];
  error: string | null;
}

export interface NormalizeResultInput {
  exitCode?: number | null;
  signal?: NodeJS.Signals | null;
  durationMs: number;
  timedOut?: boolean;
  cancelled?: boolean;
  spawnError?: Error | string | null;
  stdout: CapturedStream;
  stderr: CapturedStream;
  artifacts?: readonly ProducedArtifact[];
}

export const DEFAULT_MAX_CAPTURE_BYTES = 1024 * 1024;

export const DEFAULT_CAPTURE_POLICY: CapturePolicy = Object.freeze({
  stdout: Object.freeze({ mode: "capture", maxBytes: DEFAULT_MAX_CAPTURE_BYTES }),
  stderr: Object.freeze({ mode: "capture", maxBytes: DEFAULT_MAX_CAPTURE_BYTES }),
});

function normalizeDuration(durationMs: number): number {
  if (!Number.isFinite(durationMs)) {
    throw new TypeError("durationMs must be a finite number");
  }
  return Math.max(0, Math.round(durationMs));
}

function copyArtifact(artifact: ProducedArtifact): ProducedArtifact {
  if (!artifact.name || !artifact.path) {
    throw new TypeError("artifacts require non-empty name and path values");
  }
  return { ...artifact };
}

export function classifyExit(input: Pick<
  NormalizeResultInput,
  "exitCode" | "signal" | "timedOut" | "cancelled" | "spawnError"
>): ExitClass {
  if (input.timedOut) return "timed_out";
  if (input.cancelled) return "cancelled";
  if (input.spawnError) return "spawn_error";
  if (input.signal) return "signalled";
  if (input.exitCode === 0) return "success";
  return "failed";
}

export function normalizeProcessResult(input: NormalizeResultInput): ProcessResult {
  const timedOut = input.timedOut === true;
  const cancelled = !timedOut && input.cancelled === true;
  const spawnError = input.spawnError ?? null;

  return {
    exitClass: classifyExit({
      exitCode: input.exitCode,
      signal: input.signal,
      timedOut,
      cancelled,
      spawnError,
    }),
    exitCode: input.exitCode ?? null,
    signal: input.signal ?? null,
    durationMs: normalizeDuration(input.durationMs),
    timedOut,
    cancelled,
    stdout: {
      ...input.stdout,
      policy: { ...input.stdout.policy },
    },
    stderr: {
      ...input.stderr,
      policy: { ...input.stderr.policy },
    },
    artifacts: (input.artifacts ?? []).map(copyArtifact),
    error: spawnError instanceof Error ? spawnError.message : spawnError,
  };
}

export const normalizeResult = normalizeProcessResult;

