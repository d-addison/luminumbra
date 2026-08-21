import assert from "node:assert/strict";
import { mkdtemp, readFile, writeFile } from "node:fs/promises";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
  evaluateSimulationResult,
  runSimulationGate,
  type SimulationStepResult,
} from "../gates/simulation-gate.ts";

async function fixture(): Promise<{ directory: string; stdout: string; stderr: string; server: string }> {
  const directory = await mkdtemp(path.join(os.tmpdir(), "banso-sim-gate-"));
  const stdout = path.join(directory, "server.stdout.log");
  const stderr = path.join(directory, "server.stderr.log");
  const server = path.join(directory, "luminumbra_server");
  await Promise.all([
    writeFile(stdout, '{"world_hash":"abc123"}\n', "utf8"),
    writeFile(stderr, "", "utf8"),
    writeFile(server, "test fixture", "utf8"),
  ]);
  return { directory, stdout, stderr, server };
}

function result(
  fixturePaths: { stdout: string; stderr: string },
  overrides: Partial<SimulationStepResult> = {},
): SimulationStepResult {
  return {
    schema: "banso.sim.result.v1",
    step: "smoke",
    parameters: { arguments: ["--smoke"] },
    world_hash: "abc123",
    exit: { class: "exited-zero", code: 0, signal: null },
    evidence: { stdout: fixturePaths.stdout, stderr: fixturePaths.stderr },
    ...overrides,
  };
}

test("passes only a successful smoke result with hash and raw output evidence", async () => {
  const paths = await fixture();
  const report = await evaluateSimulationResult(result(paths), {
    serverBinary: paths.server,
    cwd: paths.directory,
  });

  assert.equal(report.status, "pass");
  assert.equal(report.world_hash, "abc123");
  assert.equal(report.evidence_dir, null);
});

test("nonzero smoke exit fails and retains a complete evidence bundle", async () => {
  const paths = await fixture();
  const report = await evaluateSimulationResult(
    result(paths, { exit: { class: "exited-nonzero", code: 7, signal: null } }),
    { serverBinary: paths.server, cwd: paths.directory, evidenceRoot: "retained" },
  );

  assert.equal(report.status, "fail");
  assert.ok(report.evidence_dir);
  assert.match(report.report, /Retained evidence/);
  assert.match(report.report, /--smoke/);
  assert.match(await readFile(path.join(report.evidence_dir, "stdout.log"), "utf8"), /world_hash/);
  assert.equal(await readFile(path.join(report.evidence_dir, "world-hash.txt"), "utf8"), "abc123\n");
  const parameters = JSON.parse(await readFile(path.join(report.evidence_dir, "run-parameters.json"), "utf8"));
  assert.deepEqual(parameters.smoke_arguments, ["--smoke"]);
  assert.equal(parameters.reproduction_command, report.reproduction_command);
});

test("missing world hash is red and evidence-bearing", async () => {
  const paths = await fixture();
  const report = await evaluateSimulationResult(result(paths, { world_hash: null }), {
    serverBinary: paths.server,
    cwd: paths.directory,
    evidenceRoot: "retained",
  });

  assert.equal(report.status, "fail");
  assert.match(report.summary, /world_hash/);
  assert.ok(report.evidence_dir);
  assert.equal(await readFile(path.join(report.evidence_dir, "world-hash.txt"), "utf8"), "null\n");
});

test("not-built server is red and retains the spawn failure", async () => {
  const paths = await fixture();
  const missingServer = path.join(paths.directory, "not-built", "luminumbra_server");
  let runnerCalled = false;
  const report = await runSimulationGate({
    serverBinary: missingServer,
    cwd: paths.directory,
    evidenceRoot: "retained",
    runSmoke: async () => {
      runnerCalled = true;
      return result(paths);
    },
  });

  assert.equal(runnerCalled, false);
  assert.equal(report.status, "fail");
  assert.match(report.summary, /could not be run/);
  assert.ok(report.evidence_dir);
  assert.match(await readFile(path.join(report.evidence_dir, "stderr.log"), "utf8"), /ENOENT/);
  const manifest = JSON.parse(await readFile(path.join(report.evidence_dir, "manifest.json"), "utf8"));
  assert.equal(manifest.retained, true);
  assert.equal(manifest.world_hash, null);
});

test("missing raw output cannot become a vacuous pass", async () => {
  const paths = await fixture();
  const report = await evaluateSimulationResult(result(paths, { evidence: {} }), {
    serverBinary: paths.server,
    cwd: paths.directory,
    evidenceRoot: "retained",
  });

  assert.equal(report.status, "fail");
  assert.match(report.summary, /output evidence is incomplete/);
  assert.ok(report.evidence_dir);
});
