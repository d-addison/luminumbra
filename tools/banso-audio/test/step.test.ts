import assert from "node:assert/strict";
import test from "node:test";

import {
  VALIDATOR_ARGS,
  VALIDATOR_COMMAND,
  runBankValidate,
  type CommandRunner,
} from "../steps/bank-validate.ts";

const PASS_REPORT = JSON.stringify({
  valid: true,
  summary: {
    manifests: 2,
    entries: 12,
    referenced_assets: 18,
    assets: 18,
    findings: 0,
  },
  findings: [],
});

const FINDINGS_REPORT = JSON.stringify({
  valid: false,
  summary: {
    manifests: 2,
    entries: 12,
    referenced_assets: 18,
    assets: 17,
    findings: 1,
  },
  findings: [
    {
      severity: "error",
      code: "missing-asset",
      message: "referenced audio asset does not exist",
      file: "data/audio/sfx_main.bank.json",
      line: 14,
      bank: "sfx_main",
      entry_id: "ui.accept",
      asset_path: "assets/audio/ui/accept.wav",
    },
  ],
});

function fixtureRunner(
  exitCode: number,
  stdout: string,
  inspect?: (command: string, args: readonly string[], cwd: string) => void,
): CommandRunner {
  return async (command, args, options) => {
    inspect?.(command, args, options.cwd);
    return { exitCode, stdout, stderr: "" };
  };
}

test("passes and returns the validator's structured report", async () => {
  const result = await runBankValidate(
    "C:/repo",
    fixtureRunner(0, PASS_REPORT, (command, args, cwd) => {
      assert.equal(command, VALIDATOR_COMMAND);
      assert.deepEqual(args, VALIDATOR_ARGS);
      assert.equal(cwd, "C:/repo");
    }),
  );

  assert.equal(result.status, "passed");
  assert.equal(result.exitCode, 0);
  assert.equal(result.output.valid, true);
  assert.deepEqual(result.output.findings, []);
});

test("fails without changing validator findings", async () => {
  const result = await runBankValidate(
    "C:/repo",
    fixtureRunner(1, FINDINGS_REPORT),
  );

  assert.equal(result.status, "failed");
  assert.equal(result.exitCode, 1);
  assert.deepEqual(result.output.findings, JSON.parse(FINDINGS_REPORT).findings);
  assert.equal(result.output.findings[0].severity, "error");
});

test("fails when the validator cannot be started", async () => {
  const missingValidator: CommandRunner = async () => {
    const error = new Error("spawn python ENOENT") as NodeJS.ErrnoException;
    error.code = "ENOENT";
    throw error;
  };

  await assert.rejects(
    runBankValidate("C:/repo", missingValidator),
    (error: unknown) =>
      error instanceof Error &&
      (error as NodeJS.ErrnoException).code === "ENOENT",
  );
});

