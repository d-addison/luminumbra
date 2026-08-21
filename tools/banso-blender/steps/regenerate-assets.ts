import { createHash } from "node:crypto";
import { spawn } from "node:child_process";
import { access, mkdir, readFile, writeFile } from "node:fs/promises";
import { dirname, extname, isAbsolute, relative, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";

export type ParameterScalar = string | number | boolean;
export type ParameterValue = ParameterScalar | ParameterScalar[];

export interface AssetFamilySidecar {
  schemaVersion: 1;
  sourceBlend: string;
  targetObject: string;
  expectedBlenderVersion: string;
  parameterMatrix: {
    seeds: number[];
    parameters: Record<string, ParameterValue[]>;
  };
  output: {
    directory: string;
    glbNamingPattern: string;
    manifest: string;
  };
}

export interface FamilyMember {
  index: number;
  seed: number;
  parameters: Record<string, ParameterValue>;
}

export interface ProcessResult {
  exitCode: number;
  stdout: string;
  stderr: string;
}

export interface RunOptions {
  cwd: string;
}

export type ProcessRunner = (
  argv: readonly string[],
  options: RunOptions,
) => Promise<ProcessResult>;

export interface RegenerationDependencies {
  runner?: ProcessRunner;
  blenderExecutable?: string;
  pythonExecutable?: string;
  bakeScript?: string;
  validatorScript?: string;
  now?: () => Date;
}

export interface OutputVerdicts {
  bake: "green" | "red";
  validator: "green" | "red";
  outputPresent: boolean;
}

export interface FamilyManifestOutput {
  index: number;
  seed: number;
  parameters: Record<string, ParameterValue>;
  path: string;
  sha256: string | null;
  bake: {
    argv: string[];
    exitCode: number;
    verdict: "green" | "red";
  };
  validator: {
    argv: string[];
    exitCode: number;
    verdict: "green" | "red";
    report: unknown;
  };
  verdict: "green" | "red";
}

export interface FamilyManifest {
  schemaVersion: 1;
  generatedAt: string;
  verdict: "green" | "red";
  error?: {
    message: string;
    remediation: string;
  };
  inputs: {
    sidecar: { path: string; sha256: string };
    sourceBlend: { path: string; sha256: string };
    targetObject: string;
    parameterMatrix: AssetFamilySidecar["parameterMatrix"];
    expectedBlenderVersion: string;
    actualBlenderVersion: string | null;
  };
  outputs: FamilyManifestOutput[];
}

export class SidecarValidationError extends Error {
  readonly issues: string[];

  constructor(issues: string[]) {
    super(`Invalid asset-family sidecar:\n${issues.map((issue) => `- ${issue}`).join("\n")}`);
    this.name = "SidecarValidationError";
    this.issues = issues;
  }
}

export class RegenerationError extends Error {
  readonly manifestPath: string;

  constructor(message: string, manifestPath: string) {
    super(message);
    this.name = "RegenerationError";
    this.manifestPath = manifestPath;
  }
}

const moduleDirectory = dirname(fileURLToPath(import.meta.url));
const defaultBakeScript = resolve(moduleDirectory, "../../blender/geonodes_bake.py");
const defaultValidatorScript = resolve(moduleDirectory, "../../blender/validate_glb.py");
const allowedPlaceholders = new Set(["index", "seed"]);

function isRecord(value: unknown): value is Record<string, unknown> {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

function rejectUnknownKeys(
  value: Record<string, unknown>,
  allowed: readonly string[],
  path: string,
  issues: string[],
): void {
  const allowedSet = new Set(allowed);
  for (const key of Object.keys(value)) {
    if (!allowedSet.has(key)) {
      issues.push(`${path}.${key} is not allowed`);
    }
  }
}

function isParameterValue(value: unknown): value is ParameterValue {
  const isScalar = (candidate: unknown): candidate is ParameterScalar =>
    typeof candidate === "string" ||
    typeof candidate === "boolean" ||
    (typeof candidate === "number" && Number.isFinite(candidate));
  return isScalar(value) ||
    (Array.isArray(value) && value.length > 0 && value.every(isScalar));
}

function isSafeRelativePath(value: string): boolean {
  if (!value || isAbsolute(value) || value.includes("\0")) {
    return false;
  }
  const normalized = resolve("C:\\sidecar-root", value);
  const rel = relative("C:\\sidecar-root", normalized);
  return rel !== ".." && !rel.startsWith(`..${sep}`) && !isAbsolute(rel);
}

function isFilename(value: string, extension: string): boolean {
  return value.length > extension.length &&
    !value.includes("/") &&
    !value.includes("\\") &&
    extname(value).toLowerCase() === extension;
}

export function parseSidecarText(text: string): AssetFamilySidecar {
  let parsed: unknown;
  try {
    parsed = JSON.parse(text);
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    throw new SidecarValidationError([`$ is not valid JSON: ${message}`]);
  }

  const issues: string[] = [];
  if (!isRecord(parsed)) {
    throw new SidecarValidationError(["$ must be an object"]);
  }
  rejectUnknownKeys(
    parsed,
    ["schemaVersion", "sourceBlend", "targetObject", "expectedBlenderVersion", "parameterMatrix", "output"],
    "$",
    issues,
  );

  if (parsed.schemaVersion !== 1) {
    issues.push("$.schemaVersion must be 1");
  }
  if (typeof parsed.sourceBlend !== "string" || !isFilename(parsed.sourceBlend, ".blend")) {
    issues.push("$.sourceBlend must be an adjacent .blend filename");
  }
  if (typeof parsed.targetObject !== "string" || !parsed.targetObject.trim()) {
    issues.push("$.targetObject must be a non-empty string");
  }
  if (
    typeof parsed.expectedBlenderVersion !== "string" ||
    !/^\d+\.\d+(?:\.\d+)?$/.test(parsed.expectedBlenderVersion)
  ) {
    issues.push("$.expectedBlenderVersion must be a major.minor or major.minor.patch version");
  }

  const matrix = isRecord(parsed.parameterMatrix) ? parsed.parameterMatrix : {};
  if (!isRecord(parsed.parameterMatrix)) {
    issues.push("$.parameterMatrix must be an object");
  } else {
    rejectUnknownKeys(matrix, ["seeds", "parameters"], "$.parameterMatrix", issues);
  }

  const seeds: number[] = [];
  if (!Array.isArray(matrix.seeds) || matrix.seeds.length === 0) {
    issues.push("$.parameterMatrix.seeds must be a non-empty array");
  } else {
    for (const [index, seed] of matrix.seeds.entries()) {
      if (!Number.isSafeInteger(seed)) {
        issues.push(`$.parameterMatrix.seeds[${index}] must be a safe integer`);
      } else {
        seeds.push(seed);
      }
    }
    if (new Set(seeds).size !== seeds.length) {
      issues.push("$.parameterMatrix.seeds must not contain duplicates");
    }
  }

  const parameters: Record<string, ParameterValue[]> = {};
  if (!isRecord(matrix.parameters)) {
    issues.push("$.parameterMatrix.parameters must be an object");
  } else {
    for (const [name, choices] of Object.entries(matrix.parameters)) {
      if (!name.trim() || name.includes("\0")) {
        issues.push("$.parameterMatrix.parameters contains an invalid parameter name");
        continue;
      }
      if (!Array.isArray(choices) || choices.length === 0) {
        issues.push(`$.parameterMatrix.parameters.${name} must be a non-empty array`);
        continue;
      }
      const accepted: ParameterValue[] = [];
      for (const [index, choice] of choices.entries()) {
        if (!isParameterValue(choice)) {
          issues.push(`$.parameterMatrix.parameters.${name}[${index}] is not a supported parameter value`);
        } else {
          accepted.push(choice);
        }
      }
      parameters[name] = accepted;
    }
  }

  const output = isRecord(parsed.output) ? parsed.output : {};
  if (!isRecord(parsed.output)) {
    issues.push("$.output must be an object");
  } else {
    rejectUnknownKeys(output, ["directory", "glbNamingPattern", "manifest"], "$.output", issues);
  }
  if (typeof output.directory !== "string" || !isSafeRelativePath(output.directory)) {
    issues.push("$.output.directory must be a safe relative path");
  }
  if (typeof output.glbNamingPattern !== "string" || !isFilename(output.glbNamingPattern, ".glb")) {
    issues.push("$.output.glbNamingPattern must be a .glb filename pattern");
  } else {
    const placeholders = [...output.glbNamingPattern.matchAll(/\{([^{}]+)\}/g)].map((match) => match[1]);
    for (const placeholder of placeholders) {
      if (!allowedPlaceholders.has(placeholder)) {
        issues.push(`$.output.glbNamingPattern contains unknown placeholder {${placeholder}}`);
      }
    }
    if (output.glbNamingPattern.replace(/\{(?:index|seed)\}/g, "").includes("{") ||
        output.glbNamingPattern.replace(/\{(?:index|seed)\}/g, "").includes("}")) {
      issues.push("$.output.glbNamingPattern contains a malformed placeholder");
    }
  }
  if (typeof output.manifest !== "string" || !isFilename(output.manifest, ".json")) {
    issues.push("$.output.manifest must be a .json filename");
  }

  if (issues.length > 0) {
    throw new SidecarValidationError(issues);
  }

  const sidecar: AssetFamilySidecar = {
    schemaVersion: 1,
    sourceBlend: parsed.sourceBlend as string,
    targetObject: parsed.targetObject as string,
    expectedBlenderVersion: parsed.expectedBlenderVersion as string,
    parameterMatrix: { seeds, parameters },
    output: {
      directory: output.directory as string,
      glbNamingPattern: output.glbNamingPattern as string,
      manifest: output.manifest as string,
    },
  };

  const names = expandParameterMatrix(sidecar).map((member) => renderOutputName(sidecar.output.glbNamingPattern, member));
  if (new Set(names).size !== names.length) {
    throw new SidecarValidationError([
      "$.output.glbNamingPattern does not produce a unique filename for every parameter set",
    ]);
  }
  return sidecar;
}

export function expandParameterMatrix(sidecar: AssetFamilySidecar): FamilyMember[] {
  const entries = Object.entries(sidecar.parameterMatrix.parameters);
  const combinations: Record<string, ParameterValue>[] = [{}];
  for (const [name, choices] of entries) {
    const next: Record<string, ParameterValue>[] = [];
    for (const existing of combinations) {
      for (const choice of choices) {
        next.push({ ...existing, [name]: choice });
      }
    }
    combinations.splice(0, combinations.length, ...next);
  }

  const members: FamilyMember[] = [];
  for (const seed of sidecar.parameterMatrix.seeds) {
    for (const parameters of combinations) {
      members.push({ index: members.length, seed, parameters: { ...parameters } });
    }
  }
  return members;
}

export function renderOutputName(pattern: string, member: FamilyMember): string {
  return pattern
    .replaceAll("{index}", String(member.index))
    .replaceAll("{seed}", String(member.seed));
}

export function buildBlenderArgv(options: {
  blenderExecutable: string;
  sourceBlend: string;
  bakeScript: string;
  targetObject: string;
  output: string;
  member: FamilyMember;
}): string[] {
  return [
    options.blenderExecutable,
    "-b",
    options.sourceBlend,
    "--python-exit-code",
    "1",
    "--python",
    options.bakeScript,
    "--",
    "--object",
    options.targetObject,
    "--output",
    options.output,
    "--seed",
    String(options.member.seed),
    "--params-json",
    JSON.stringify(options.member.parameters),
  ];
}

export function buildValidatorArgv(options: {
  pythonExecutable: string;
  validatorScript: string;
  output: string;
}): string[] {
  return [options.pythonExecutable, options.validatorScript, "--json", options.output];
}

export function aggregateVerdict(
  outputs: readonly OutputVerdicts[],
  prerequisitesGreen = true,
): "green" | "red" {
  return prerequisitesGreen &&
    outputs.length > 0 &&
    outputs.every((output) =>
      output.bake === "green" && output.validator === "green" && output.outputPresent)
    ? "green"
    : "red";
}

export const defaultProcessRunner: ProcessRunner = (argv, options) =>
  new Promise((resolveProcess, rejectProcess) => {
    if (argv.length === 0) {
      rejectProcess(new Error("Cannot run an empty argv"));
      return;
    }
    const child = spawn(argv[0], argv.slice(1), {
      cwd: options.cwd,
      shell: false,
      windowsHide: true,
      stdio: ["ignore", "pipe", "pipe"],
    });
    let stdout = "";
    let stderr = "";
    child.stdout.setEncoding("utf8");
    child.stderr.setEncoding("utf8");
    child.stdout.on("data", (chunk: string) => { stdout += chunk; });
    child.stderr.on("data", (chunk: string) => { stderr += chunk; });
    child.on("error", rejectProcess);
    child.on("close", (code) => {
      resolveProcess({ exitCode: code ?? 1, stdout, stderr });
    });
  });

async function sha256(path: string): Promise<string> {
  return createHash("sha256").update(await readFile(path)).digest("hex");
}

async function pathExists(path: string): Promise<boolean> {
  try {
    await access(path);
    return true;
  } catch {
    return false;
  }
}

function parseBlenderVersion(output: string): string | null {
  return /(?:^|\s)Blender\s+(\d+\.\d+\.\d+)/m.exec(output)?.[1] ?? null;
}

function versionMatches(expected: string, actual: string): boolean {
  return actual === expected || actual.startsWith(`${expected}.`);
}

function parseValidatorReport(stdout: string): unknown {
  try {
    return JSON.parse(stdout);
  } catch {
    return null;
  }
}

function validatorAccepted(result: ProcessResult, report: unknown): boolean {
  return result.exitCode === 0 && isRecord(report) && report.valid === true;
}

async function writeManifest(path: string, manifest: FamilyManifest): Promise<void> {
  await mkdir(dirname(path), { recursive: true });
  await writeFile(path, `${JSON.stringify(manifest, null, 2)}\n`, "utf8");
}

function unavailableRemediation(executable: string, expectedVersion: string): string {
  return `Install Blender ${expectedVersion}, then put '${executable}' on PATH or pass --blender with its executable path.`;
}

async function runOrFailure(
  runner: ProcessRunner,
  argv: string[],
  cwd: string,
): Promise<ProcessResult> {
  try {
    return await runner(argv, { cwd });
  } catch (error) {
    const message = error instanceof Error ? error.message : String(error);
    return { exitCode: 1, stdout: "", stderr: message };
  }
}

export async function regenerateAssets(
  sidecarPath: string,
  dependencies: RegenerationDependencies = {},
): Promise<FamilyManifest> {
  const absoluteSidecar = resolve(sidecarPath);
  const sidecarDirectory = dirname(absoluteSidecar);
  const sidecar = parseSidecarText(await readFile(absoluteSidecar, "utf8"));
  const sourceBlend = resolve(sidecarDirectory, sidecar.sourceBlend);
  if (!(await pathExists(sourceBlend))) {
    throw new SidecarValidationError([`$.sourceBlend does not exist beside the sidecar: ${sidecar.sourceBlend}`]);
  }

  const outputDirectory = resolve(sidecarDirectory, sidecar.output.directory);
  const manifestPath = resolve(outputDirectory, sidecar.output.manifest);
  const runner = dependencies.runner ?? defaultProcessRunner;
  const blenderExecutable = dependencies.blenderExecutable ?? process.env.BLENDER_EXECUTABLE ?? "blender";
  const pythonExecutable = dependencies.pythonExecutable ?? process.env.PYTHON_EXECUTABLE ?? "python";
  const bakeScript = resolve(dependencies.bakeScript ?? defaultBakeScript);
  const validatorScript = resolve(dependencies.validatorScript ?? defaultValidatorScript);
  const manifest: FamilyManifest = {
    schemaVersion: 1,
    generatedAt: (dependencies.now ?? (() => new Date()))().toISOString(),
    verdict: "red",
    inputs: {
      sidecar: { path: absoluteSidecar, sha256: await sha256(absoluteSidecar) },
      sourceBlend: { path: sourceBlend, sha256: await sha256(sourceBlend) },
      targetObject: sidecar.targetObject,
      parameterMatrix: sidecar.parameterMatrix,
      expectedBlenderVersion: sidecar.expectedBlenderVersion,
      actualBlenderVersion: null,
    },
    outputs: [],
  };

  let probe: ProcessResult;
  try {
    probe = await runner([blenderExecutable, "--version"], { cwd: sidecarDirectory });
  } catch (error) {
    const detail = error instanceof Error ? error.message : String(error);
    const remediation = unavailableRemediation(blenderExecutable, sidecar.expectedBlenderVersion);
    manifest.error = { message: `Blender is not installed or could not be started: ${detail}`, remediation };
    await writeManifest(manifestPath, manifest);
    throw new RegenerationError(`${manifest.error.message}\nRemediation: ${remediation}`, manifestPath);
  }

  const actualVersion = parseBlenderVersion(`${probe.stdout}\n${probe.stderr}`);
  manifest.inputs.actualBlenderVersion = actualVersion;
  if (probe.exitCode !== 0 || actualVersion === null) {
    const remediation = unavailableRemediation(blenderExecutable, sidecar.expectedBlenderVersion);
    manifest.error = { message: "Blender is not installed or its version could not be determined.", remediation };
    await writeManifest(manifestPath, manifest);
    throw new RegenerationError(`${manifest.error.message}\nRemediation: ${remediation}`, manifestPath);
  }
  if (!versionMatches(sidecar.expectedBlenderVersion, actualVersion)) {
    const remediation = unavailableRemediation(blenderExecutable, sidecar.expectedBlenderVersion);
    manifest.error = {
      message: `Blender ${actualVersion} does not match expected version ${sidecar.expectedBlenderVersion}.`,
      remediation,
    };
    await writeManifest(manifestPath, manifest);
    throw new RegenerationError(`${manifest.error.message}\nRemediation: ${remediation}`, manifestPath);
  }

  await mkdir(outputDirectory, { recursive: true });
  for (const member of expandParameterMatrix(sidecar)) {
    const outputPath = resolve(outputDirectory, renderOutputName(sidecar.output.glbNamingPattern, member));
    const bakeArgv = buildBlenderArgv({
      blenderExecutable,
      sourceBlend,
      bakeScript,
      targetObject: sidecar.targetObject,
      output: outputPath,
      member,
    });
    const bakeResult = await runOrFailure(runner, bakeArgv, sidecarDirectory);
    const validatorArgv = buildValidatorArgv({ pythonExecutable, validatorScript, output: outputPath });
    const validatorResult = await runOrFailure(runner, validatorArgv, sidecarDirectory);
    const validatorReport = parseValidatorReport(validatorResult.stdout);
    const outputPresent = await pathExists(outputPath);
    const outputHash = outputPresent ? await sha256(outputPath) : null;
    const bakeVerdict = bakeResult.exitCode === 0 ? "green" : "red";
    const validatorVerdict = validatorAccepted(validatorResult, validatorReport) ? "green" : "red";
    const verdict = aggregateVerdict([
      { bake: bakeVerdict, validator: validatorVerdict, outputPresent },
    ]);
    manifest.outputs.push({
      index: member.index,
      seed: member.seed,
      parameters: member.parameters,
      path: outputPath,
      sha256: outputHash,
      bake: { argv: bakeArgv, exitCode: bakeResult.exitCode, verdict: bakeVerdict },
      validator: {
        argv: validatorArgv,
        exitCode: validatorResult.exitCode,
        verdict: validatorVerdict,
        report: validatorReport,
      },
      verdict,
    });
  }

  manifest.verdict = aggregateVerdict(manifest.outputs.map((output) => ({
    bake: output.bake.verdict,
    validator: output.validator.verdict,
    outputPresent: output.sha256 !== null,
  })));
  await writeManifest(manifestPath, manifest);
  if (manifest.verdict === "red") {
    throw new RegenerationError(
      `Asset-family regeneration failed; inspect the red verdicts in ${manifestPath}`,
      manifestPath,
    );
  }
  return manifest;
}

function parseCommandLine(argv: string[]): {
  sidecar: string;
  blenderExecutable?: string;
  pythonExecutable?: string;
} {
  let sidecar: string | undefined;
  let blenderExecutable: string | undefined;
  let pythonExecutable: string | undefined;
  for (let index = 0; index < argv.length; index += 1) {
    const flag = argv[index];
    const value = argv[index + 1];
    if (flag === "--sidecar" || flag === "--blender" || flag === "--python") {
      if (value === undefined) {
        throw new Error(`${flag} requires a value`);
      }
      if (flag === "--sidecar") sidecar = value;
      if (flag === "--blender") blenderExecutable = value;
      if (flag === "--python") pythonExecutable = value;
      index += 1;
    } else {
      throw new Error(`Unknown argument: ${flag}`);
    }
  }
  if (!sidecar) {
    throw new Error("Usage: regenerate-assets --sidecar FAMILY.json [--blender PATH] [--python PATH]");
  }
  return { sidecar, blenderExecutable, pythonExecutable };
}

async function main(): Promise<void> {
  const options = parseCommandLine(process.argv.slice(2));
  const manifest = await regenerateAssets(options.sidecar, options);
  process.stdout.write(`${JSON.stringify({ verdict: manifest.verdict, outputs: manifest.outputs.length })}\n`);
}

if (process.argv[1] && resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch((error) => {
    process.stderr.write(`${error instanceof Error ? error.message : String(error)}\n`);
    process.exitCode = 1;
  });
}
