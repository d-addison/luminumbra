import { randomUUID } from "node:crypto";
import { join } from "node:path";
import { createRunManifest, writeRunManifest } from "../lib/manifest.ts";
import type { ExitClass, ProducedArtifact } from "../lib/result.ts";
import { createCommand, supervise } from "../lib/supervisor.ts";

export interface UiScreenshotArguments {
  screen?: string;
  directory?: string;
  fixtures?: boolean;
}

export interface UiScreenshotParameters extends UiScreenshotArguments {
  executable: string;
  cwd?: string;
  timeoutMs?: number;
  runId?: string;
  manifestPath?: string;
  environment?: NodeJS.ProcessEnv;
}

export interface UiScreenshotOutput {
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

export function buildUiScreenshotArgs(parameters: UiScreenshotArguments = {}): string[] {
  const screen = optionalText(parameters.screen, "screen");
  const directory = optionalText(parameters.directory, "directory");
  const args = ["--ui-screenshot"];
  if (screen !== undefined) args.push(screen);
  if (directory !== undefined) args.push("--ui-screenshot-dir", directory);
  if (parameters.fixtures === true) args.push("--ui-fixtures");
  return args;
}

export function uiScreenshotArtifacts(parameters: UiScreenshotArguments = {}): ProducedArtifact[] {
  const screen = optionalText(parameters.screen, "screen");
  if (screen === undefined) return [];
  const directory = optionalText(parameters.directory, "directory") ?? "references/compare";
  return screen.split(",").filter((name) => name.length > 0).map((name) => ({
    name: `ui-${name}`,
    path: join(directory, `ui-${name}.ppm`),
    kind: "image/x-portable-pixmap",
  }));
}

export async function runUiScreenshot(parameters: UiScreenshotParameters): Promise<UiScreenshotOutput> {
  const command = createCommand(parameters.executable, buildUiScreenshotArgs(parameters));
  const cwd = optionalText(parameters.cwd, "cwd") ?? process.cwd();
  const runId = optionalText(parameters.runId, "runId") ?? randomUUID();
  const manifestPath = optionalText(parameters.manifestPath, "manifestPath")
    ?? join(cwd, ".banso", "runs", runId, "manifest.json");
  const environment = parameters.environment ?? process.env;
  const artifacts = uiScreenshotArtifacts(parameters);
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
    step: "client.ui-screenshot",
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

export default runUiScreenshot;
