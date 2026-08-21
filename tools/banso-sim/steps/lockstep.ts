import { makeResult, observeProcess, type NormalizedResult } from "./smoke.ts";

export interface LockstepParameters {
  executable: string;
  seed: string;
  ticks: string;
  evidence_directory: string;
  artifact_paths?: readonly string[];
}

export function lockstepArgv(
  parameters: Pick<LockstepParameters, "seed" | "ticks">,
): string[] {
  return [
    "--lockstep-loopback",
    "--seed",
    parameters.seed,
    "--ticks",
    parameters.ticks,
  ];
}

export function runLockstep(parameters: LockstepParameters): NormalizedResult {
  const argv = lockstepArgv(parameters);
  const observation = observeProcess(
    parameters.executable,
    argv,
    parameters.evidence_directory,
    "lockstep-loopback",
  );

  return makeResult(
    "lockstep-loopback",
    {
      executable: parameters.executable,
      seed: parameters.seed,
      ticks: parameters.ticks,
      argv,
    },
    [observation],
    parameters.artifact_paths,
  );
}

export default runLockstep;
