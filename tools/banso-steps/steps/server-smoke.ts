import { randomUUID } from "node:crypto";
import { join } from "node:path";
import { createRunManifest, writeRunManifest } from "../lib/manifest.ts";
import type { ExitClass, ProducedArtifact } from "../lib/result.ts";
import { createCommand, supervise } from "../lib/supervisor.ts";

export interface ServerSmokeArguments {
  seed?: string;
  ticks?: number;
  artifact?: string;
}

export interface ServerSmokeParameters extends ServerSmokeArguments {
  executable: string;
  cwd?: string;
  timeoutMs?: number;
  runId?: string;
  manifestPath?: string;
  environment?: NodeJS.ProcessEnv;
}

export interface ServerSmokeOutput {
  runId: string;
  exitClass: ExitClass;
  exitCode: number | null;
  manifestPath: string;
  artifacts: ProducedArtifact[];
}

function optionalText(value: string | undefined, label: string): string | undefined {
  if (value === undefined) return undefined;
  if (typeof value !== "string" || value.length === 0) {
    throw new TypeError(`${label} must be a non-empty string`);
  }
  return value;
}

function optionalTicks(value: number | undefined): number | undefined {
  if (value === undefined) return undefined;
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new RangeError("ticks must be a non-negative safe integer");
  }
  return value;
}

export function buildServerSmokeArgs(parameters: ServerSmokeArguments = {}): string[] {
  const seed = optionalText(parameters.seed, "seed");
  const ticks = optionalTicks(parameters.ticks);
  const artifact = optionalText(parameters.artifact, "artifact");
  const args = ["--smoke"];
  if (seed !== undefined) args.push("--seed", seed);
  if (ticks !== undefined) args.push("--ticks", String(ticks));
  if (artifact !== undefined) args.push("--artifact", artifact);
  return args;
}

export function serverSmokeArtifacts(parameters: ServerSmokeArguments = {}): ProducedArtifact[] {
  const artifact = optionalText(parameters.artifact, "artifact");
  return artifact === undefined
    ? []
    : [{ name: "server-smoke", path: artifact, kind: "application/json" }];
}

export async function runServerSmoke(parameters: ServerSmokeParameters): Promise<ServerSmokeOutput> {
  const command = createCommand(parameters.executable, buildServerSmokeArgs(parameters));
  const cwd = optionalText(parameters.cwd, "cwd") ?? process.cwd();
  const runId = optionalText(parameters.runId, "runId") ?? randomUUID();
  const manifestPath = optionalText(parameters.manifestPath, "manifestPath")
    ?? join(cwd, ".banso", "runs", runId, "manifest.json");
  const environment = parameters.environment ?? process.env;
  const artifacts = serverSmokeArtifacts(parameters);
  const startedAt = new Date();
  const result = await supervise({
    executable: command.executable,
    args: command.args,
    cwd,
    env: environment,
    timeoutMs: parameters.timeoutMs,
    artifacts,
  });
  const finishedAt = new Date();
  const manifest = createRunManifest({
    runId,
    step: "server.smoke",
    command,
    cwd,
    environment,
    startedAt,
    finishedAt,
    timeoutMs: parameters.timeoutMs,
    result,
  });
  await writeRunManifest(manifestPath, manifest);
  return {
    runId,
    exitClass: result.exitClass,
    exitCode: result.exitCode,
    manifestPath,
    artifacts: result.artifacts,
  };
}

export default runServerSmoke;
