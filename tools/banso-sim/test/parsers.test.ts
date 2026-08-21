import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

import { lockstepArgv } from "../steps/lockstep.ts";
import { replayArgv } from "../steps/replay.ts";
import {
  classifyExit,
  extractWorldHash,
  smokeArgv,
} from "../steps/smoke.ts";

const fixture = readFileSync(
  new URL("./fixtures/smoke-output.txt", import.meta.url),
  "utf8",
);

function transcript(name: string): string {
  const marker = `===== ${name} =====`;
  const start = fixture.indexOf(marker);
  assert.notEqual(start, -1, `missing fixture transcript ${name}`);
  const contentStart = start + marker.length;
  const next = fixture.indexOf("=====", contentStart);
  return fixture.slice(contentStart, next === -1 ? undefined : next).trim();
}

test("extracts the successful smoke world_hash verbatim", () => {
  assert.equal(
    extractWorldHash(transcript("smoke-success")),
    "6f008a9f637c40b7",
  );
});

test("does not normalize a world_hash in a determinism failure", () => {
  assert.equal(
    extractWorldHash(transcript("smoke-determinism-failure")),
    "6f008a9f637c40B7-RAW",
  );
});

test("returns null for a process failure transcript without a world_hash", () => {
  assert.equal(extractWorldHash(transcript("smoke-process-failure")), null);
});

test("preserves textual world_hash spelling and selects the final emitted token", () => {
  const output = "world_hash=First_RAW\nworld_hash: 'Second-Raw_Value'\n";
  assert.equal(extractWorldHash(output), "Second-Raw_Value");
});

test("classifies raw exit states without consulting server output", () => {
  assert.deepEqual(classifyExit(0, null), {
    exit_class: "exited-zero",
    exit_code: 0,
    signal: null,
    error: null,
  });
  assert.deepEqual(classifyExit(7, null), {
    exit_class: "exited-nonzero",
    exit_code: 7,
    signal: null,
    error: null,
  });
  assert.deepEqual(classifyExit(null, "SIGTERM"), {
    exit_class: "signaled",
    exit_code: null,
    signal: "SIGTERM",
    error: null,
  });
  assert.deepEqual(classifyExit(null, null, new Error("spawn failed")), {
    exit_class: "spawn-error",
    exit_code: null,
    signal: null,
    error: "spawn failed",
  });
});

test("maps smoke parameters to argv with no additions or reordering", () => {
  const parameters = { seed: "000042", ticks: "00120" };
  const before = structuredClone(parameters);

  assert.deepEqual(smokeArgv(parameters), [
    "--smoke",
    "--seed",
    "000042",
    "--ticks",
    "00120",
  ]);
  assert.deepEqual(parameters, before);
});
test("maps replay parameters to record then replay argv exactly", () => {
  const parameters = {
    seed: "seed-as-supplied",
    ticks: "ticks-as-supplied",
    recording_path: "evidence/session.lrec1",
  };
  const before = structuredClone(parameters);

  assert.deepEqual(replayArgv(parameters), {
    record: [
      "--record",
      "evidence/session.lrec1",
      "--seed",
      "seed-as-supplied",
      "--ticks",
      "ticks-as-supplied",
    ],
    replay: ["--replay", "evidence/session.lrec1"],
  });
  assert.deepEqual(parameters, before);
});

test("maps lockstep parameters to argv with no additions or reordering", () => {
  const parameters = { seed: "Seed-RAW", ticks: "Ticks-RAW" };
  const before = structuredClone(parameters);

  assert.deepEqual(lockstepArgv(parameters), [
    "--lockstep-loopback",
    "--seed",
    "Seed-RAW",
    "--ticks",
    "Ticks-RAW",
  ]);
  assert.deepEqual(parameters, before);
});
