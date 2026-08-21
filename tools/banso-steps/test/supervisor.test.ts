import assert from "node:assert/strict";
import { mkdtemp, readFile, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import test from "node:test";
import { createRunManifest, writeRunManifest } from "../lib/manifest.ts";
import {
  DEFAULT_CAPTURE_POLICY,
  normalizeProcessResult,
  type CapturedStream,
} from "../lib/result.ts";
import { buildArgv, createCommand, runProcess } from "../lib/supervisor.ts";

const emptyCapture = (text = ""): CapturedStream => ({
  policy: { ...DEFAULT_CAPTURE_POLICY.stdout },
  text,
  bytes: Buffer.byteLength(text),
  truncated: false,
});

test("buildArgv and the supervisor preserve exact arguments without shell expansion", async () => {
  const exactArguments = [
    "two words",
    "$HOME",
    "$(echo injected)",
    "semi;colon",
    "quote\"and'apostrophe",
    "star*question?",
  ];

  assert.deepEqual(buildArgv(process.execPath, exactArguments), [process.execPath, ...exactArguments]);

  const result = await runProcess({
    executable: process.execPath,
    args: [
      "-e",
      "process.stdout.write(JSON.stringify(process.argv.slice(1)))",
      "--",
      ...exactArguments,
    ],
    timeoutMs: 2_000,
  });

  assert.equal(result.exitClass, "success");
  assert.equal(result.stderr.text, "");
  assert.deepEqual(JSON.parse(result.stdout.text ?? "null"), exactArguments);
});

test("timeout terminates a running child and is normalized as timed_out", async () => {
  const result = await runProcess({
    executable: process.execPath,
    args: ["-e", "process.stdout.write('started'); setInterval(() => {}, 1000)"],
    timeoutMs: 100,
    killGraceMs: 100,
  });

  assert.equal(result.exitClass, "timed_out");
  assert.equal(result.timedOut, true);
  assert.equal(result.cancelled, false);
  assert.match(result.stdout.text ?? "", /^started$/);
  assert.ok(result.durationMs >= 50);
  assert.ok(result.durationMs < 5_000);
});

test("an AbortSignal cancels and terminates a running child", async () => {
  const controller = new AbortController();
  const running = runProcess({
    executable: process.execPath,
    args: ["-e", "setInterval(() => {}, 1000)"],
    signal: controller.signal,
    timeoutMs: 5_000,
  });
  setTimeout(() => controller.abort(), 100);

  const result = await running;
  assert.equal(result.exitClass, "cancelled");
  assert.equal(result.cancelled, true);
  assert.equal(result.timedOut, false);
});

test("result normalization classifies failures and preserves capture and artifacts", () => {
  const result = normalizeProcessResult({
    exitCode: 7,
    durationMs: 12.6,
    stdout: emptyCapture("output"),
    stderr: emptyCapture("problem"),
    artifacts: [{
      name: "report",
      path: "runs/example/report.json",
      kind: "application/json",
      bytes: 42,
      sha256: "a".repeat(64),
    }],
  });

  assert.deepEqual(result, {
    exitClass: "failed",
    exitCode: 7,
    signal: null,
    durationMs: 13,
    timedOut: false,
    cancelled: false,
    stdout: emptyCapture("output"),
    stderr: emptyCapture("problem"),
    artifacts: [{
      name: "report",
      path: "runs/example/report.json",
      kind: "application/json",
      bytes: 42,
      sha256: "a".repeat(64),
    }],
    error: null,
  });
});

test("manifest has a stable shape, exact argv, redacted environment, and artifacts", async () => {
  const result = normalizeProcessResult({
    exitCode: 0,
    durationMs: 27,
    stdout: emptyCapture("ok"),
    stderr: emptyCapture(),
    artifacts: [{ name: "capture", path: "runs/abc/frame.png", kind: "image/png" }],
  });
  const command = createCommand("luminumbra client", ["--ui-screenshot", "main menu"]);
  const manifest = createRunManifest({
    runId: "run-abc",
    step: "capture.ui",
    command,
    cwd: "C:\\work root",
    environment: { MODE: "capture", API_TOKEN: "do-not-record" },
    startedAt: "2026-01-02T03:04:05.000Z",
    finishedAt: "2026-01-02T03:04:05.027Z",
    timeoutMs: 2_000,
    result,
    build: { commit: "abc123" },
    host: { gpu: "test-adapter" },
  });

  assert.equal(manifest.schemaVersion, 1);
  assert.deepEqual(manifest.command.argv, [
    "luminumbra client",
    "--ui-screenshot",
    "main menu",
  ]);
  assert.deepEqual(manifest.command.environment, {
    API_TOKEN: "[REDACTED]",
    MODE: "capture",
  });
  assert.equal(manifest.result.exitClass, "success");
  assert.equal(manifest.result.stdout.policy.mode, "capture");
  assert.equal(manifest.artifacts[0].name, "capture");

  const directory = await mkdtemp(join(tmpdir(), "banso-manifest-"));
  try {
    const path = join(directory, "nested", "run-manifest.json");
    await writeRunManifest(path, manifest);
    const written = JSON.parse(await readFile(path, "utf8"));
    assert.deepEqual(written, manifest);
  } finally {
    await rm(directory, { recursive: true, force: true });
  }
});
