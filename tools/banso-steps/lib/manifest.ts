import { mkdir, rename, writeFile } from "node:fs/promises";
import { dirname, basename, join } from "node:path";
import { randomUUID } from "node:crypto";
import type { CommandSpec } from "./supervisor.ts";
import type { CapturedStream, ProcessResult, ProducedArtifact } from "./result.ts";

export const RUN_MANIFEST_SCHEMA_VERSION = 1;
export const REDACTED_ENVIRONMENT_VALUE = "[REDACTED]";

export interface RunManifestCommand {
  executable: string;
  args: string[];
  argv: string[];
  cwd: string;
  environment: Record<string, string>;
}

export interface RunManifestTiming {
  startedAt: string;
  finishedAt: string;
  durationMs: number;
  timeoutMs: number | null;
}

export interface RunManifestStream {
  policy: CapturedStream["policy"];
  bytes: number;
  truncated: boolean;
}

export interface RunManifestResult {
  exitClass: ProcessResult["exitClass"];
  exitCode: number | null;
  signal: NodeJS.Signals | null;
  timedOut: boolean;
  cancelled: boolean;
  error: string | null;
  stdout: RunManifestStream;
  stderr: RunManifestStream;
}

export interface RunManifest {
  schemaVersion: typeof RUN_MANIFEST_SCHEMA_VERSION;
  runId: string;
  step: string;
  build: Record<string, string>;
  command: RunManifestCommand;
  timing: RunManifestTiming;
  result: RunManifestResult;
  artifacts: ProducedArtifact[];
  host: Record<string, string>;
}

export interface RunManifestInput {
  runId: string;
  step: string;
  command: CommandSpec;
  cwd: string;
  environment?: NodeJS.ProcessEnv;
  startedAt: Date | string;
  finishedAt: Date | string;
  timeoutMs?: number | null;
  result: ProcessResult;
  build?: Record<string, string>;
  host?: Record<string, string>;
}

const SENSITIVE_ENVIRONMENT_KEY = /(?:auth|cookie|credential|key|pass|secret|token)/i;

function isoTimestamp(value: Date | string, label: string): string {
  const date = value instanceof Date ? value : new Date(value);
  if (Number.isNaN(date.valueOf())) {
    throw new TypeError(`${label} must be a valid date`);
  }
  return date.toISOString();
}

export function redactEnvironment(environment: NodeJS.ProcessEnv = {}): Record<string, string> {
  const entries = Object.entries(environment)
    .filter((entry): entry is [string, string] => entry[1] !== undefined)
    .sort(([left], [right]) => left.localeCompare(right))
    .map(([key, value]) => [
      key,
      SENSITIVE_ENVIRONMENT_KEY.test(key) ? REDACTED_ENVIRONMENT_VALUE : value,
    ]);
  return Object.fromEntries(entries);
}

function manifestStream(stream: CapturedStream): RunManifestStream {
  return {
    policy: { ...stream.policy },
    bytes: stream.bytes,
    truncated: stream.truncated,
  };
}

export function createRunManifest(input: RunManifestInput): RunManifest {
  if (!input.runId || !input.step || !input.cwd) {
    throw new TypeError("runId, step, and cwd must be non-empty");
  }

  const startedAt = isoTimestamp(input.startedAt, "startedAt");
  const finishedAt = isoTimestamp(input.finishedAt, "finishedAt");

  return {
    schemaVersion: RUN_MANIFEST_SCHEMA_VERSION,
    runId: input.runId,
    step: input.step,
    build: { ...(input.build ?? {}) },
    command: {
      executable: input.command.executable,
      args: [...input.command.args],
      argv: [input.command.executable, ...input.command.args],
      cwd: input.cwd,
      environment: redactEnvironment(input.environment),
    },
    timing: {
      startedAt,
      finishedAt,
      durationMs: input.result.durationMs,
      timeoutMs: input.timeoutMs ?? null,
    },
    result: {
      exitClass: input.result.exitClass,
      exitCode: input.result.exitCode,
      signal: input.result.signal,
      timedOut: input.result.timedOut,
      cancelled: input.result.cancelled,
      error: input.result.error,
      stdout: manifestStream(input.result.stdout),
      stderr: manifestStream(input.result.stderr),
    },
    artifacts: input.result.artifacts.map((artifact) => ({ ...artifact })),
    host: { ...(input.host ?? {}) },
  };
}

export async function writeRunManifest(path: string, manifest: RunManifest): Promise<void> {
  const directory = dirname(path);
  await mkdir(directory, { recursive: true });
  const temporaryPath = join(directory, `.${basename(path)}.${process.pid}.${randomUUID()}.tmp`);
  await writeFile(temporaryPath, `${JSON.stringify(manifest, null, 2)}\n`, {
    encoding: "utf8",
    flag: "wx",
  });
  await rename(temporaryPath, path);
}

export const writeManifest = writeRunManifest;

