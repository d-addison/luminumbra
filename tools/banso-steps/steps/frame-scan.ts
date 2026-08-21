import { randomUUID } from "node:crypto";
import { join } from "node:path";
import { createRunManifest, writeRunManifest } from "../lib/manifest.ts";
import type { ExitClass, ProducedArtifact } from "../lib/result.ts";
import { createCommand, supervise } from "../lib/supervisor.ts";

export interface FrameScanArguments {
  output?: string;
}

export interface FrameScanParameters extends FrameScanArguments {
  executable: string;
  cwd?: string;
  timeoutMs?: number;
  runId?: string;
  manifestPath?: string;
  environment?: NodeJS.ProcessEnv;
}

export interface FrameScanOutput {
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

export function buildFrameScanArgs(parameters: FrameScanArguments = {}): string[] {
  const output = optionalText(parameters.output, "output");
  return output === undefined ? ["--frame-scan"] : ["--frame-scan", output];
}

export function frameScanArtifacts(parameters: FrameScanArguments = {}): ProducedArtifact[] {
  const output = optionalText(parameters.output, "output");
  return output === undefined
    ? []
    : [{ name: "frame-scan", path: output, kind: "application/json" }];
}

export async function runFrameScan(parameters: FrameScanParameters): Promise<FrameScanOutput> {
  const command = createCommand(parameters.executable, buildFrameScanArgs(parameters));
  const cwd = optionalText(parameters.cwd, "cwd") ?? process.cwd();
  const runId = optionalText(parameters.runId, "runId") ?? randomUUID();
  const manifestPath = optionalText(parameters.manifestPath, "manifestPath")
    ?? join(cwd, ".banso", "runs", runId, "manifest.json");
  const environment = parameters.environment ?? process.env;
  const artifacts = frameScanArtifacts(parameters);
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
    step: "client.frame-scan",
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

export default runFrameScan;
