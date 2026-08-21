import assert from "node:assert/strict";
import { join } from "node:path";
import test from "node:test";
import {
  buildFrameScanArgs,
  frameScanArtifacts,
} from "../steps/frame-scan.ts";
import {
  buildRenderBenchmarkArgs,
  renderBenchmarkArtifacts,
} from "../steps/render-benchmark.ts";
import {
  buildServerSmokeArgs,
  serverSmokeArtifacts,
} from "../steps/server-smoke.ts";
import {
  buildUiScreenshotArgs,
  uiScreenshotArtifacts,
} from "../steps/ui-screenshot.ts";

test("ui screenshot preserves the bare flag and appends only supplied options", () => {
  assert.deepEqual(buildUiScreenshotArgs(), ["--ui-screenshot"]);
  assert.deepEqual(buildUiScreenshotArgs({
    screen: "world_creation,gallery",
    directory: "captures/ui",
    fixtures: true,
  }), [
    "--ui-screenshot",
    "world_creation,gallery",
    "--ui-screenshot-dir",
    "captures/ui",
    "--ui-fixtures",
  ]);
  assert.deepEqual(uiScreenshotArtifacts({ screen: "main_menu" }), [{
    name: "ui-main_menu",
    path: join("references/compare", "ui-main_menu.ppm"),
    kind: "image/x-portable-pixmap",
  }]);
});

test("render benchmark preserves the bare flag and exact explicit tuning", () => {
  assert.deepEqual(buildRenderBenchmarkArgs(), ["--render-benchmark"]);
  assert.deepEqual(buildRenderBenchmarkArgs({
    output: "artifacts/render.json",
    frames: 240,
    warmupFrames: 90,
    screenshot: "artifacts/render.ppm",
  }), [
    "--render-benchmark",
    "artifacts/render.json",
    "--render-benchmark-frames",
    "240",
    "--render-benchmark-warmup",
    "90",
    "--render-benchmark-screenshot",
    "artifacts/render.ppm",
  ]);
  assert.deepEqual(renderBenchmarkArtifacts({ output: "bench.json" }), [{
    name: "render-benchmark",
    path: "bench.json",
    kind: "application/json",
  }]);
});

test("frame scan preserves the bare flag and exact output path", () => {
  assert.deepEqual(buildFrameScanArgs(), ["--frame-scan"]);
  assert.deepEqual(buildFrameScanArgs({ output: "artifacts/frame scan.json" }), [
    "--frame-scan",
    "artifacts/frame scan.json",
  ]);
  assert.deepEqual(frameScanArtifacts({ output: "scan.json" }), [{
    name: "frame-scan",
    path: "scan.json",
    kind: "application/json",
  }]);
});

test("server smoke preserves the bare flag and exact seed, ticks, and artifact", () => {
  assert.deepEqual(buildServerSmokeArgs(), ["--smoke"]);
  assert.deepEqual(buildServerSmokeArgs({
    seed: "000042",
    ticks: 600,
    artifact: "artifacts/smoke.json",
  }), [
    "--smoke",
    "--seed",
    "000042",
    "--ticks",
    "600",
    "--artifact",
    "artifacts/smoke.json",
  ]);
  assert.deepEqual(serverSmokeArtifacts({ artifact: "smoke.json" }), [{
    name: "server-smoke",
    path: "smoke.json",
    kind: "application/json",
  }]);
});
