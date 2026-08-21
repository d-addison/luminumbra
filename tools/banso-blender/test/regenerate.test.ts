import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { mkdtemp, readFile, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join, resolve } from "node:path";
import test from "node:test";

import {
  RegenerationError,
  SidecarValidationError,
  aggregateVerdict,
  buildBlenderArgv,
  expandParameterMatrix,
  parseSidecarText,
  regenerateAssets,
  type AssetFamilySidecar,
  type ProcessRunner,
} from "../steps/regenerate-assets.ts";

const validFixture = {
  schemaVersion: 1,
  sourceBlend: "forest.blend",
  targetObject: "ProceduralTree",
  expectedBlenderVersion: "4.3",
  parameterMatrix: {
    seeds: [11, 29],
    parameters: {
      Height: [4, 8],
      Wind: [false, true],
    },
  },
  output: {
    directory: "generated",
    glbNamingPattern: "tree-{index}-seed-{seed}.glb",
    manifest: "tree-family.manifest.json",
  },
} satisfies AssetFamilySidecar;

function fixtureText(overrides: Record<string, unknown> = {}): string {
  return JSON.stringify({ ...validFixture, ...overrides });
}

async function writeFamilyFixture(
  sidecar: AssetFamilySidecar = validFixture,
): Promise<{ root: string; sidecarPath: string }> {
  const root = await mkdtemp(join(tmpdir(), "banso-blender-"));
  const sidecarPath = join(root, "forest.asset-family.json");
  await writeFile(join(root, sidecar.sourceBlend), "blend fixture", "utf8");
  await writeFile(sidecarPath, JSON.stringify(sidecar), "utf8");
  return { root, sidecarPath };
}

test("parses the versioned sidecar fixture", () => {
  const parsed = parseSidecarText(fixtureText());
  assert.equal(parsed.schemaVersion, 1);
  assert.equal(parsed.sourceBlend, "forest.blend");
  assert.deepEqual(parsed.parameterMatrix.seeds, [11, 29]);
});

test("reports clear paths for malformed sidecars", () => {
  assert.throws(
    () => parseSidecarText(fixtureText({
      schemaVersion: 2,
      sourceBlend: "../forest.blend",
      parameterMatrix: { seeds: [], parameters: { Height: [] } },
    })),
    (error: unknown) => {
      assert.ok(error instanceof SidecarValidationError);
      assert.match(error.message, /\$\.schemaVersion must be 1/);
      assert.match(error.message, /\$\.sourceBlend must be an adjacent \.blend filename/);
      assert.match(error.message, /\$\.parameterMatrix\.seeds must be a non-empty array/);
      assert.match(error.message, /\$\.parameterMatrix\.parameters\.Height must be a non-empty array/);
      return true;
    },
  );
});

test("expands seeds and parameter choices as a stable Cartesian product", () => {
  const sidecar = parseSidecarText(fixtureText());
  const members = expandParameterMatrix(sidecar);
  assert.equal(members.length, 8);
  assert.deepEqual(members[0], { index: 0, seed: 11, parameters: { Height: 4, Wind: false } });
  assert.deepEqual(members[3], { index: 3, seed: 11, parameters: { Height: 8, Wind: true } });
  assert.deepEqual(members[4], { index: 4, seed: 29, parameters: { Height: 4, Wind: false } });
});

test("constructs the geometry-nodes adapter argv exactly", () => {
  const member = { index: 3, seed: 29, parameters: { Height: 8, Wind: true } };
  assert.deepEqual(buildBlenderArgv({
    blenderExecutable: "C:\\Blender\\blender.exe",
    sourceBlend: "C:\\family\\forest.blend",
    bakeScript: "C:\\repo\\tools\\blender\\geonodes_bake.py",
    targetObject: "ProceduralTree",
    output: "C:\\family\\generated\\tree-3.glb",
    member,
  }), [
    "C:\\Blender\\blender.exe",
    "-b",
    "C:\\family\\forest.blend",
    "--python-exit-code",
    "1",
    "--python",
    "C:\\repo\\tools\\blender\\geonodes_bake.py",
    "--",
    "--object",
    "ProceduralTree",
    "--output",
    "C:\\family\\generated\\tree-3.glb",
    "--seed",
    "29",
    "--params-json",
    "{\"Height\":8,\"Wind\":true}",
  ]);
});

test("aggregates any missing output or red member to a red family verdict", () => {
  assert.equal(aggregateVerdict([
    { bake: "green", validator: "green", outputPresent: true },
  ]), "green");
  assert.equal(aggregateVerdict([
    { bake: "green", validator: "green", outputPresent: true },
    { bake: "green", validator: "red", outputPresent: true },
  ]), "red");
  assert.equal(aggregateVerdict([
    { bake: "green", validator: "green", outputPresent: false },
  ]), "red");
  assert.equal(aggregateVerdict([], true), "red");
});

test("fails red with remediation and a manifest when Blender is absent", async () => {
  const fixture = await writeFamilyFixture();
  const missingRunner: ProcessRunner = async () => {
    const error = new Error("spawn blender ENOENT") as Error & { code: string };
    error.code = "ENOENT";
    throw error;
  };
  await assert.rejects(
    regenerateAssets(fixture.sidecarPath, {
      runner: missingRunner,
      now: () => new Date("2026-01-02T03:04:05.000Z"),
    }),
    (error: unknown) => {
      assert.ok(error instanceof RegenerationError);
      assert.match(error.message, /Blender is not installed/);
      assert.match(error.message, /Remediation: Install Blender 4\.3/);
      return true;
    },
  );
  const manifestPath = join(fixture.root, "generated", "tree-family.manifest.json");
  const manifest = JSON.parse(await readFile(manifestPath, "utf8"));
  assert.equal(manifest.verdict, "red");
  assert.match(manifest.error.remediation, /put 'blender' on PATH/);
  assert.deepEqual(manifest.outputs, []);
});

test("invokes the tested seam once per member, validates each GLB, and hashes outputs", async () => {
  const compactFixture: AssetFamilySidecar = {
    ...validFixture,
    parameterMatrix: { seeds: [5, 7], parameters: { Height: [4] } },
  };
  const fixture = await writeFamilyFixture(compactFixture);
  const calls: string[][] = [];
  const runner: ProcessRunner = async (argv) => {
    calls.push([...argv]);
    if (argv[1] === "--version") {
      return { exitCode: 0, stdout: "Blender 4.3.2\n", stderr: "" };
    }
    if (argv.includes("--python-exit-code")) {
      const output = argv[argv.indexOf("--output") + 1];
      await writeFile(output, `glb-${argv[argv.indexOf("--seed") + 1]}`, "utf8");
      return { exitCode: 0, stdout: "{}\n", stderr: "" };
    }
    return {
      exitCode: 0,
      stdout: JSON.stringify({ valid: true, profile: "static", findings: [] }),
      stderr: "",
    };
  };

  const manifest = await regenerateAssets(fixture.sidecarPath, {
    runner,
    blenderExecutable: "blender-test",
    pythonExecutable: "python-test",
    bakeScript: join(fixture.root, "geonodes_bake.py"),
    validatorScript: join(fixture.root, "validate_glb.py"),
    now: () => new Date("2026-01-02T03:04:05.000Z"),
  });

  assert.equal(calls.filter((argv) => argv.includes("--python-exit-code")).length, 2);
  assert.equal(calls.filter((argv) => argv.some((argument) => argument.endsWith("validate_glb.py"))).length, 2);
  assert.equal(manifest.verdict, "green");
  assert.equal(manifest.outputs.length, 2);
  assert.deepEqual(manifest.outputs.map((output) => output.validator.verdict), ["green", "green"]);
  assert.equal(
    manifest.outputs[0].sha256,
    createHash("sha256").update("glb-5").digest("hex"),
  );
  const persisted = JSON.parse(await readFile(join(fixture.root, "generated", "tree-family.manifest.json"), "utf8"));
  assert.equal(persisted.inputs.sourceBlend.sha256.length, 64);
  assert.equal(persisted.outputs[1].sha256.length, 64);
});

test("persists all validator verdicts and fails the step when any member is red", async () => {
  const compactFixture: AssetFamilySidecar = {
    ...validFixture,
    parameterMatrix: { seeds: [5, 7], parameters: {} },
  };
  const fixture = await writeFamilyFixture(compactFixture);
  const runner: ProcessRunner = async (argv) => {
    if (argv[1] === "--version") {
      return { exitCode: 0, stdout: "Blender 4.3.1\n", stderr: "" };
    }
    if (argv.includes("--python-exit-code")) {
      const output = argv[argv.indexOf("--output") + 1];
      await writeFile(output, "glb", "utf8");
      return { exitCode: 0, stdout: "", stderr: "" };
    }
    const isSecond = argv.at(-1)?.includes("seed-7") ?? false;
    return {
      exitCode: isSecond ? 1 : 0,
      stdout: JSON.stringify({ valid: !isSecond, findings: isSecond ? [{ severity: "error" }] : [] }),
      stderr: "",
    };
  };

  await assert.rejects(
    regenerateAssets(fixture.sidecarPath, { runner }),
    RegenerationError,
  );
  const manifest = JSON.parse(await readFile(join(fixture.root, "generated", "tree-family.manifest.json"), "utf8"));
  assert.equal(manifest.verdict, "red");
  assert.deepEqual(manifest.outputs.map((output: { validator: { verdict: string } }) => output.validator.verdict), ["green", "red"]);
  assert.equal(manifest.outputs[1].validator.report.valid, false);
});
