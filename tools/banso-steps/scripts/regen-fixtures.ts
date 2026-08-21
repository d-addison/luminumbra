import { mkdir, writeFile } from "node:fs/promises";
import { dirname, resolve } from "node:path";
import { fileURLToPath, pathToFileURL } from "node:url";
import {
  buildFrameScanArgs,
  type FrameScanArguments,
} from "../steps/frame-scan.ts";
import {
  buildRenderBenchmarkArgs,
  type RenderBenchmarkArguments,
} from "../steps/render-benchmark.ts";
import {
  buildServerSmokeArgs,
  type ServerSmokeArguments,
} from "../steps/server-smoke.ts";
import {
  buildUiScreenshotArgs,
  type UiScreenshotArguments,
} from "../steps/ui-screenshot.ts";

interface ParameterSet<T> {
  name: string;
  parameters: T;
}

export interface ArgvSnapshot {
  step: string;
  case: string;
  parameters: object;
  argv: string[];
}

export interface ArgvSnapshotFixture {
  schemaVersion: 1;
  snapshots: ArgvSnapshot[];
}

const uiScreenshotCases: ParameterSet<UiScreenshotArguments>[] = [
  { name: "defaults", parameters: {} },
  { name: "screen", parameters: { screen: "world_creation,gallery" } },
  { name: "directory", parameters: { directory: "captures/ui" } },
  { name: "fixtures", parameters: { fixtures: true } },
  {
    name: "all-options",
    parameters: {
      screen: "world_creation,gallery",
      directory: "artifacts/ui captures",
      fixtures: true,
    },
  },
];

const renderBenchmarkCases: ParameterSet<RenderBenchmarkArguments>[] = [
  { name: "defaults", parameters: {} },
  { name: "output", parameters: { output: "artifacts/render.json" } },
  { name: "zero-frames", parameters: { frames: 0 } },
  { name: "zero-warmup", parameters: { warmupFrames: 0 } },
  { name: "screenshot", parameters: { screenshot: "artifacts/render.ppm" } },
  {
    name: "all-options",
    parameters: {
      output: "artifacts/render benchmark.json",
      frames: 240,
      warmupFrames: 90,
      screenshot: "artifacts/render benchmark.ppm",
    },
  },
];

const frameScanCases: ParameterSet<FrameScanArguments>[] = [
  { name: "defaults", parameters: {} },
  { name: "output", parameters: { output: "artifacts/frame scan.json" } },
];

const serverSmokeCases: ParameterSet<ServerSmokeArguments>[] = [
  { name: "defaults", parameters: {} },
  { name: "seed", parameters: { seed: "000042" } },
  { name: "zero-ticks", parameters: { ticks: 0 } },
  { name: "artifact", parameters: { artifact: "artifacts/smoke.json" } },
  {
    name: "all-options",
    parameters: {
      seed: "000042",
      ticks: 600,
      artifact: "artifacts/server smoke.json",
    },
  },
];

function snapshotsFor<T extends object>(
  step: string,
  cases: ParameterSet<T>[],
  buildArgs: (parameters: T) => string[],
): ArgvSnapshot[] {
  return cases.map(({ name, parameters }) => ({
    step,
    case: name,
    parameters,
    argv: buildArgs(parameters),
  }));
}

export function buildArgvSnapshots(): ArgvSnapshotFixture {
  return {
    schemaVersion: 1,
    snapshots: [
      ...snapshotsFor("client.ui-screenshot", uiScreenshotCases, buildUiScreenshotArgs),
      ...snapshotsFor("client.render-benchmark", renderBenchmarkCases, buildRenderBenchmarkArgs),
      ...snapshotsFor("client.frame-scan", frameScanCases, buildFrameScanArgs),
      ...snapshotsFor("server.smoke", serverSmokeCases, buildServerSmokeArgs),
    ],
  };
}

export const argvSnapshotsFixturePath = resolve(
  dirname(fileURLToPath(import.meta.url)),
  "../test/fixtures/argv-snapshots.json",
);

async function regenerateFixtures(): Promise<void> {
  const contents = `${JSON.stringify(buildArgvSnapshots(), null, 2)}\n`;
  await mkdir(dirname(argvSnapshotsFixturePath), { recursive: true });
  await writeFile(argvSnapshotsFixturePath, contents, "utf8");
  process.stdout.write(`Wrote ${argvSnapshotsFixturePath}\n`);
}

const entryPath = process.argv[1];
if (entryPath !== undefined && import.meta.url === pathToFileURL(resolve(entryPath)).href) {
  await regenerateFixtures();
}
