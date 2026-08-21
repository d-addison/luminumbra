import { spawn } from "node:child_process";
import { resolve } from "node:path";
import { pathToFileURL } from "node:url";

export const VALIDATOR_COMMAND = "python";
export const VALIDATOR_ARGS = Object.freeze([
  "tools/audio/bank_validate.py",
  "--json",
] as const);

export interface ValidatorFinding {
  severity: string;
  code: string;
  message: string;
  file: string;
  line: number;
  bank: string | null;
  entry_id: string | null;
  asset_path: string | null;
}

export interface ValidatorSummary {
  manifests: number;
  entries: number;
  referenced_assets: number;
  assets: number;
  findings: number;
}

export interface ValidatorReport {
  valid: boolean;
  summary: ValidatorSummary;
  findings: ValidatorFinding[];
}

export interface CommandResult {
  exitCode: number;
  stdout: string;
  stderr: string;
}

export type CommandRunner = (
  command: string,
  args: readonly string[],
  options: { cwd: string },
) => Promise<CommandResult>;

export interface BankValidateResult {
  status: "passed" | "failed";
  output: ValidatorReport;
  exitCode: number;
  stderr: string;
}

export class ValidatorOutputError extends Error {
  constructor(message: string, options?: ErrorOptions) {
    super(message, options);
    this.name = "ValidatorOutputError";
  }
}

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

export function parseValidatorReport(stdout: string): ValidatorReport {
  let value: unknown;
  try {
    value = JSON.parse(stdout);
  } catch (error) {
    throw new ValidatorOutputError("audio bank validator did not emit valid JSON", {
      cause: error,
    });
  }

  if (
    !isRecord(value) ||
    typeof value.valid !== "boolean" ||
    !isRecord(value.summary) ||
    !Array.isArray(value.findings)
  ) {
    throw new ValidatorOutputError(
      "audio bank validator JSON does not contain a report",
    );
  }

  return value as unknown as ValidatorReport;
}

export const executeCommand: CommandRunner = (command, args, options) =>
  new Promise((resolveCommand, rejectCommand) => {
    const child = spawn(command, [...args], {
      cwd: options.cwd,
      shell: false,
      windowsHide: true,
      stdio: ["ignore", "pipe", "pipe"],
    });
    let stdout = "";
    let stderr = "";

    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (chunk: string) => {
      stdout += chunk;
    });
    child.stderr.on("data", (chunk: string) => {
      stderr += chunk;
    });
    child.once("error", rejectCommand);
    child.once("close", (exitCode, signal) => {
      if (signal !== null) {
        rejectCommand(
          new Error(`audio bank validator terminated by signal ${signal}`),
        );
        return;
      }
      resolveCommand({ exitCode: exitCode ?? 1, stdout, stderr });
    });
  });

export async function runBankValidate(
  cwd: string,
  runner: CommandRunner = executeCommand,
): Promise<BankValidateResult> {
  const result = await runner(VALIDATOR_COMMAND, VALIDATOR_ARGS, { cwd });
  const output = parseValidatorReport(result.stdout);

  return {
    status: result.exitCode === 0 ? "passed" : "failed",
    output,
    exitCode: result.exitCode,
    stderr: result.stderr,
  };
}

async function main(): Promise<void> {
  try {
    const result = await runBankValidate(process.cwd());
    process.stdout.write(`${JSON.stringify(result.output)}\n`);
    if (result.stderr.length > 0) {
      process.stderr.write(result.stderr);
    }
    process.exitCode = result.exitCode === 0 ? 0 : result.exitCode;
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    process.stderr.write(
      `${JSON.stringify({ error: "audio-bank-validate", message })}\n`,
    );
    process.exitCode = 1;
  }
}

const invokedPath = process.argv[1];
if (
  invokedPath !== undefined &&
  pathToFileURL(resolve(invokedPath)).href === import.meta.url
) {
  void main();
}

