import {
  makeResult,
  observeProcess,
  type NormalizedResult,
  type ProcessObservation,
} from "./smoke.ts";

export interface ReplayParameters {
  executable: string;
  seed: string;
  ticks: string;
  recording_path: string;
  evidence_directory: string;
  artifact_paths?: readonly string[];
}

export interface ReplayArgv {
  record: string[];
  replay: string[];
}

export function replayArgv(
  parameters: Pick<ReplayParameters, "seed" | "ticks" | "recording_path">,
): ReplayArgv {
  return {
    record: [
      "--record",
      parameters.recording_path,
      "--seed",
      parameters.seed,
      "--ticks",
      parameters.ticks,
    ],
    replay: ["--replay", parameters.recording_path],
  };
}

export function runReplay(parameters: ReplayParameters): NormalizedResult {
  const argv = replayArgv(parameters);
  const record = observeProcess(
    parameters.executable,
    argv.record,
    parameters.evidence_directory,
    "replay-record",
  );
  const observations: ProcessObservation[] = [record];

  if (record.exit.exit_class === "exited-zero") {
    observations.push(
      observeProcess(
        parameters.executable,
        argv.replay,
        parameters.evidence_directory,
        "replay-playback",
      ),
    );
  }

  const artifacts = [parameters.recording_path, ...(parameters.artifact_paths ?? [])];

  return makeResult(
    "replay",
    {
      executable: parameters.executable,
      seed: parameters.seed,
      ticks: parameters.ticks,
      recording_path: parameters.recording_path,
      record_argv: argv.record,
      replay_argv: argv.replay,
    },
    observations,
    artifacts,
  );
}

export default runReplay;
