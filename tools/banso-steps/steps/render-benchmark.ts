import { randomUUID } from "node:crypto";
import { join } from "node:path";
import { createRunManifest, writeRunManifest } from "../lib/manifest.ts";
import type { ExitClass, ProducedArtifact } from "../lib/result.ts";
import { createCommand, supervise } from "../lib/supervisor.ts";

export interface RenderBenchmarkArguments {
  output?: string;
  frames?: number;
  warmupFrames?: number;
  screenshot?: string;
}

export interface RenderBenchmarkParameters extends RenderBenchmarkArguments {
  executable: string;
  cwd?: string;
  timeoutMs?: number;
  runId?: string;
  manifestPath?: string;
  environment?: NodeJS.ProcessEnv;
}

export interface RenderBenchmarkOutput {
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

function optionalCount(value: number | undefined, label: string): number | undefined {
  if (value === undefined) return undefined;
  if (!Number.isSafeInteger(value) || value < 0) {
    throw new RangeError(`${label} must be a non-negative safe integer`);
  }
  return value;
}

export function buildRenderBenchmarkArgs(parameters: RenderBenchmarkArguments = {}): string[] {
  const output = optionalText(parameters.output, "output");
  const frames = optionalCount(parameters.frames, "frames");
  const warmupFrames = optionalCount(parameters.warmupFrames, "warmupFrames");
  const screenshot = optionalText(parameters.screenshot, "screenshot");
  const args = ["--render-benchmark"];
  if (output !== undefined) args.push(output);
  if (frames !== undefined) args.push("--render-benchmark-frames", String(frames));
  if (warmupFrames !== undefined) args.push("--render-benchmark-warmup", String(warmupFrames));
  if (screenshot !== undefined) args.push("--render-benchmark-screenshot", screenshot);
  return args;
}

export function renderBenchmarkArtifacts(parameters: RenderBenchmarkArguments = {}): ProducedArtifact[] {
  const artifacts: ProducedArtifact[] = [];
  const output = optionalText(parameters.output, "output");
  const screenshot = optionalText(parameters.screenshot, "screenshot");
  if (output !== undefined) {
    artifacts.push({ name: "render-benchmark", path: output, kind: "application/json" });
  }
  if (screenshot !== undefined) {
    artifacts.push({ name: "render-benchmark-screenshot", path: screenshot, kind: "image/x-portable-pixmap" });
  }
  return artifacts;
}

export async function runRenderBenchmark(parameters: RenderBenchmarkParameters): Promise<RenderBenchmarkOutput> {
  const command = createCommand(parameters.executable, buildRenderBenchmarkArgs(parameters));
  const cwd = optionalText(parameters.cwd, "cwd") ?? process.cwd();
  const runId = optionalText(parameters.runId, "runId") ?? randomUUID();
  const manifestPath = optionalText(parameters.manifestPath, "manifestPath")
    ?? join(cwd, ".banso", "runs", runId, "manifest.json");
  const environment = parameters.environment ?? process.env;
  const artifacts = renderBenchmarkArtifacts(parameters);
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
    step: "client.render-benchmark",
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

export default runRenderBenchmark;
