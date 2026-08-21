import { spawnSync } from "node:child_process";
import { mkdirSync, writeFileSync } from "node:fs";
import { join } from "node:path";

export type SimulationStep = "smoke" | "replay" | "lockstep-loopback";

export type ExitClass =
  | "exited-zero"
  | "exited-nonzero"
  | "signaled"
  | "spawn-error";

export interface ExitState {
  exit_class: ExitClass;
  exit_code: number | null;
  signal: string | null;
  error: string | null;
}

export interface EvidencePaths {
  stdout: string[];
  stderr: string[];
  artifacts: string[];
}

export interface NormalizedResult {
  schema: "banso.sim.result.v1";
  step: SimulationStep;
  parameters: Record<string, unknown>;
  world_hash: string | null;
  exit: ExitState;
  evidence: EvidencePaths;
}

export interface ProcessObservation {
  argv: string[];
  stdout: string;
  stderr: string;
  exit: ExitState;
  stdout_path: string;
  stderr_path: string;
}

export interface SmokeParameters {
  executable: string;
  seed: string;
  ticks: string;
  evidence_directory: string;
  artifact_paths?: readonly string[];
}

const WORLD_HASH_PATTERN =
  /(?:^|[\s,{])(?:"world_hash"|world_hash)\s*(?:=|:)\s*(?:"([^"\r\n]*)"|'([^'\r\n]*)'|([^\s,=}\]]+))/gm;

/** Returns the last world_hash token exactly as it appeared in the transcript. */
export function extractWorldHash(output: string): string | null {
  let worldHash: string | null = null;

  for (const match of output.matchAll(WORLD_HASH_PATTERN)) {
    worldHash = match[1] ?? match[2] ?? match[3] ?? null;
  }

  return worldHash;
}

/** Classifies only the operating-system process state; server output is not reinterpreted. */
export function classifyExit(
  exitCode: number | null,
  signal: string | null,
  error: Error | undefined = undefined,
): ExitState {
  if (error !== undefined) {
    return {
      exit_class: "spawn-error",
      exit_code: exitCode,
      signal,
      error: error.message,
    };
  }

  if (signal !== null) {
    return {
      exit_class: "signaled",
      exit_code: exitCode,
      signal,
      error: null,
    };
  }

  return {
    exit_class: exitCode === 0 ? "exited-zero" : "exited-nonzero",
    exit_code: exitCode,
    signal: null,
    error: null,
  };
}

export function observeProcess(
  executable: string,
  argv: readonly string[],
  evidenceDirectory: string,
  evidenceStem: string,
): ProcessObservation {
  mkdirSync(evidenceDirectory, { recursive: true });

  const capturedArgv = [...argv];
  const process = spawnSync(executable, capturedArgv, {
    encoding: "utf8",
    shell: false,
    windowsHide: true,
    maxBuffer: 64 * 1024 * 1024,
  });
  const stdout = process.stdout ?? "";
  const stderr = process.stderr ?? "";
  const stdoutPath = join(evidenceDirectory, `${evidenceStem}.stdout.log`);
  const stderrPath = join(evidenceDirectory, `${evidenceStem}.stderr.log`);

  writeFileSync(stdoutPath, stdout, "utf8");
  writeFileSync(stderrPath, stderr, "utf8");

  return {
    argv: capturedArgv,
    stdout,
    stderr,
    exit: classifyExit(process.status, process.signal, process.error),
    stdout_path: stdoutPath,
    stderr_path: stderrPath,
  };
}

export function smokeArgv(parameters: Pick<SmokeParameters, "seed" | "ticks">): string[] {
  return ["--smoke", "--seed", parameters.seed, "--ticks", parameters.ticks];
}

export function makeResult(
  step: SimulationStep,
  parameters: Record<string, unknown>,
  observations: readonly ProcessObservation[],
  artifacts: readonly string[] = [],
): NormalizedResult {
  const transcript = observations
    .map((observation) => `${observation.stdout}\n${observation.stderr}`)
    .join("\n");
  const finalObservation = observations.at(-1);

  if (finalObservation === undefined) {
    throw new Error("A normalized result requires at least one process observation");
  }

  return {
    schema: "banso.sim.result.v1",
    step,
    parameters,
    world_hash: extractWorldHash(transcript),
    exit: finalObservation.exit,
    evidence: {
      stdout: observations.map((observation) => observation.stdout_path),
      stderr: observations.map((observation) => observation.stderr_path),
      artifacts: [...artifacts],
    },
  };
}

export function runSmoke(parameters: SmokeParameters): NormalizedResult {
  const argv = smokeArgv(parameters);
  const observation = observeProcess(
    parameters.executable,
    argv,
    parameters.evidence_directory,
    "smoke",
  );

  return makeResult(
    "smoke",
    {
      executable: parameters.executable,
      seed: parameters.seed,
      ticks: parameters.ticks,
      argv,
    },
    [observation],
    parameters.artifact_paths,
  );
}

export default runSmoke;
