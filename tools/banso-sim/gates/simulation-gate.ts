import { access, mkdir, readFile, writeFile } from "node:fs/promises";
import path from "node:path";
import process from "node:process";
import { fileURLToPath } from "node:url";

type UnknownRecord = Record<string, unknown>;

export type SimulationStepResult = {
  schema?: unknown;
  step?: unknown;
  parameters?: unknown;
  world_hash?: unknown;
  exit?: unknown;
  evidence?: unknown;
  [key: string]: unknown;
};

export type SimulationGateOptions = {
  serverBinary: string;
  evidenceRoot?: string;
  smokeArgs?: string[];
  cwd?: string;
  runSmoke?: (parameters: UnknownRecord) => Promise<SimulationStepResult>;
};

export type SimulationGateReport = {
  schema: "banso.gate.result.v1";
  gate: "simulation";
  status: "pass" | "fail";
  summary: string;
  reproduction_command: string;
  world_hash: string | null;
  evidence_dir: string | null;
  report: string;
  reasons: string[];
  result: SimulationStepResult;
};

type EvidenceText = {
  present: boolean;
  source: string | null;
  text: string;
};

function isRecord(value: unknown): value is UnknownRecord {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function firstString(record: UnknownRecord, keys: string[]): string | null {
  for (const key of keys) {
    const value = record[key];
    if (typeof value === "string" && value.length > 0) {
      return value;
    }
  }
  return null;
}

function exitClass(result: SimulationStepResult): string | null {
  if (!isRecord(result.exit)) {
    return null;
  }
  return firstString(result.exit, ["class", "exit_class", "kind"]);
}

function exitCode(result: SimulationStepResult): number | null {
  if (!isRecord(result.exit)) {
    return null;
  }
  for (const key of ["code", "exit_code", "status"]) {
    const value = result.exit[key];
    if (typeof value === "number" && Number.isInteger(value)) {
      return value;
    }
  }
  return null;
}

function normalizedWorldHash(result: SimulationStepResult): string | null {
  return typeof result.world_hash === "string" && result.world_hash.trim().length > 0
    ? result.world_hash
    : null;
}

function quotePowerShell(value: string): string {
  return `'${value.replaceAll("'", "''")}'`;
}

function reproductionCommand(options: SimulationGateOptions): string {
  return [options.serverBinary, "--smoke", ...(options.smokeArgs ?? [])]
    .map(quotePowerShell)
    .join(" ");
}

function evidenceCandidate(result: SimulationStepResult, stream: "stdout" | "stderr"): unknown {
  if (!isRecord(result.evidence)) {
    return undefined;
  }
  const keys = [stream, `${stream}_path`, `${stream}Path`];
  for (const key of keys) {
    const value = result.evidence[key];
    if (value === undefined) {
      continue;
    }
    if (Array.isArray(value)) {
      const last = value.at(-1);
      if (typeof last === "string") {
        return last;
      }
      continue;
    }
    return value;
  }
  return undefined;
}

async function readEvidenceText(
  result: SimulationStepResult,
  stream: "stdout" | "stderr",
  cwd: string,
): Promise<EvidenceText> {
  const candidate = evidenceCandidate(result, stream);
  if (typeof candidate === "string" && candidate.length > 0) {
    const source = path.resolve(cwd, candidate);
    try {
      return { present: true, source, text: await readFile(source, "utf8") };
    } catch {
      return {
        present: false,
        source,
        text: `[${stream} evidence missing at ${source}]\n`,
      };
    }
  }

  if (isRecord(result.evidence)) {
    const raw = firstString(result.evidence, [`raw_${stream}`, `raw${stream[0].toUpperCase()}${stream.slice(1)}`]);
    if (raw !== null) {
      return { present: true, source: null, text: raw };
    }
  }

  return { present: false, source: null, text: `[${stream} evidence was not provided]\n` };
}

function verdictReasons(
  result: SimulationStepResult,
  stdout: EvidenceText,
  stderr: EvidenceText,
): string[] {
  const reasons: string[] = [];
  const kind = exitClass(result);
  const code = exitCode(result);

  if (kind !== "exited-zero") {
    reasons.push(kind === "spawn-error" ? "server binary could not be run" : `smoke process did not exit successfully (${kind ?? "missing exit state"})`);
  }
  if (code !== 0) {
    reasons.push(`smoke process exit code was ${code ?? "missing"}`);
  }
  if (normalizedWorldHash(result) === null) {
    reasons.push("smoke result did not contain a world_hash");
  }
  if (!stdout.present || !stderr.present) {
    reasons.push("raw server output evidence is incomplete");
  }
  return reasons;
}

function safeTimestamp(): string {
  return new Date().toISOString().replaceAll(":", "-").replaceAll(".", "-");
}

async function retainFailureEvidence(
  options: SimulationGateOptions,
  result: SimulationStepResult,
  stdout: EvidenceText,
  stderr: EvidenceText,
  reasons: string[],
  command: string,
): Promise<string> {
  const cwd = options.cwd ?? process.cwd();
  const root = path.resolve(cwd, options.evidenceRoot ?? ".banso/evidence/simulation");
  const directory = path.join(root, `failure-${safeTimestamp()}-${process.pid}`);
  await mkdir(directory, { recursive: true });

  const parameters = {
    server_binary: path.resolve(cwd, options.serverBinary),
    smoke_arguments: ["--smoke", ...(options.smokeArgs ?? [])],
    working_directory: path.resolve(cwd),
    reproduction_command: command,
    normalized_parameters: result.parameters ?? null,
  };
  const manifest = {
    schema: "banso.simulation.evidence.v1",
    retained: true,
    verdict: "fail",
    reasons,
    world_hash: normalizedWorldHash(result),
    reproduction_command: command,
    files: ["stdout.log", "stderr.log", "run-parameters.json", "world-hash.txt", "result.json"],
    source_evidence: {
      stdout: stdout.source,
      stderr: stderr.source,
    },
  };

  await Promise.all([
    writeFile(path.join(directory, "stdout.log"), stdout.text, "utf8"),
    writeFile(path.join(directory, "stderr.log"), stderr.text, "utf8"),
    writeFile(path.join(directory, "run-parameters.json"), `${JSON.stringify(parameters, null, 2)}\n`, "utf8"),
    writeFile(path.join(directory, "world-hash.txt"), `${normalizedWorldHash(result) ?? "null"}\n`, "utf8"),
    writeFile(path.join(directory, "result.json"), `${JSON.stringify(result, null, 2)}\n`, "utf8"),
    writeFile(path.join(directory, "manifest.json"), `${JSON.stringify(manifest, null, 2)}\n`, "utf8"),
  ]);
  return directory;
}

export async function evaluateSimulationResult(
  result: SimulationStepResult,
  options: SimulationGateOptions,
): Promise<SimulationGateReport> {
  const cwd = options.cwd ?? process.cwd();
  const command = reproductionCommand(options);
  const [stdout, stderr] = await Promise.all([
    readEvidenceText(result, "stdout", cwd),
    readEvidenceText(result, "stderr", cwd),
  ]);
  const reasons = verdictReasons(result, stdout, stderr);

  if (reasons.length === 0) {
    return {
      schema: "banso.gate.result.v1",
      gate: "simulation",
      status: "pass",
      summary: `simulation smoke passed with world_hash ${normalizedWorldHash(result)}`,
      reproduction_command: command,
      world_hash: normalizedWorldHash(result),
      evidence_dir: null,
      report: "Simulation smoke passed.",
      reasons: [],
      result,
    };
  }

  const directory = await retainFailureEvidence(options, result, stdout, stderr, reasons, command);
  return {
    schema: "banso.gate.result.v1",
    gate: "simulation",
    status: "fail",
    summary: reasons.join("; "),
    reproduction_command: command,
    world_hash: normalizedWorldHash(result),
    evidence_dir: directory,
    report: `Simulation smoke failed. [Retained evidence](${directory}) Reproduce with: ${command}`,
    reasons,
    result,
  };
}

async function defaultSmokeRunner(parameters: UnknownRecord): Promise<SimulationStepResult> {
  const module = await import("../steps/smoke.ts");
  return await module.runSmoke(parameters) as SimulationStepResult;
}

function spawnErrorResult(options: SimulationGateOptions, error: unknown): SimulationStepResult {
  const message = error instanceof Error ? error.stack ?? error.message : String(error);
  return {
    schema: "banso.sim.result.v1",
    step: "smoke",
    parameters: {
      server_binary: options.serverBinary,
      arguments: ["--smoke", ...(options.smokeArgs ?? [])],
      cwd: options.cwd ?? process.cwd(),
    },
    world_hash: null,
    exit: {
      class: "spawn-error",
      code: null,
      signal: null,
      error: message,
    },
    evidence: {
      raw_stdout: "",
      raw_stderr: `${message}\n`,
    },
  };
}

export async function runSimulationGate(options: SimulationGateOptions): Promise<SimulationGateReport> {
  const cwd = options.cwd ?? process.cwd();
  const evidenceRoot = path.resolve(cwd, options.evidenceRoot ?? ".banso/evidence/simulation");
  const smokeArgs = options.smokeArgs ?? [];
  const readFlag = (name: string, fallback: string): string => {
    const index = smokeArgs.lastIndexOf(name);
    const value = index >= 0 ? smokeArgs.at(index + 1) : undefined;
    return value ?? fallback;
  };
  const parameters: UnknownRecord = {
    executable: path.resolve(cwd, options.serverBinary),
    seed: readFlag("--seed", "1"),
    ticks: readFlag("--ticks", "300"),
    evidence_directory: evidenceRoot,
  };

  let result: SimulationStepResult;
  try {
    await access(path.resolve(cwd, options.serverBinary));
    result = await (options.runSmoke ?? defaultSmokeRunner)(parameters);
  } catch (error) {
    result = spawnErrorResult(options, error);
  }
  return await evaluateSimulationResult(result, options);
}

function parseCommandLine(argv: string[]): SimulationGateOptions {
  let serverBinary = process.env.LUMINUMBRA_SERVER ?? "";
  let evidenceRoot: string | undefined;
  const smokeArgs: string[] = [];

  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === "--server") {
      serverBinary = argv[++index] ?? "";
    } else if (argument === "--evidence-dir") {
      evidenceRoot = argv[++index];
    } else if (argument === "--") {
      smokeArgs.push(...argv.slice(index + 1));
      break;
    } else {
      smokeArgs.push(argument);
    }
  }

  if (serverBinary.length === 0) {
    serverBinary = "luminumbra_server";
  }
  return { serverBinary, evidenceRoot, smokeArgs };
}

async function main(): Promise<void> {
  const report = await runSimulationGate(parseCommandLine(process.argv.slice(2)));
  process.stdout.write(`${JSON.stringify(report, null, 2)}\n`);
  if (report.status === "fail") {
    process.exitCode = 1;
  }
}

const invokedPath = process.argv[1] ? path.resolve(process.argv[1]) : "";
if (invokedPath === path.resolve(fileURLToPath(import.meta.url))) {
  await main();
}
