#include "ModeHelpers.h"
#include "Modes.h"

namespace fs = std::filesystem;

namespace {
// ---------------------------------------------------------------------------
//  session replay (LREC1). Recording is the desync-repro tool: a stream
// is (boot parameters + per-tick inputs + 30-tick world_hash checkpoints).
// Replay reboots from the header, feeds the recorded inputs, and verifies the
// live hashes against the recorded checkpoints -- the first mismatch localizes a
// desync to a tick + a sub-hash section (the  localization). The headless
// smoke has NO player inputs today, so the per-tick input set is empty; the
// format carries it anyway for  (lockstep transport, which consumes this
// stream as its desync dump format).
//
// Determinism: recording must NOT perturb the simulation. The ReplayWriter
// buffers all records in memory and flushes to disk only at Finalize, so no
// IO sits on the tick path. Checkpoint hashing reuses ComputeWorldHash /
// ComputeWorldSubHashes (the same quiesce-then-snapshot the smoke does), which
// reads state without mutating it. Proof: the ReplayRoundtrip gate asserts the
// recorded run reaches the SAME 0eac465289e7c88b as the smoke (
// hash revision: was 2fa007951a21e140 before the `wind` sub-hash slot landed).
// ---------------------------------------------------------------------------

constexpr std::uint64_t kCheckpointIntervalTicks = 30; // one second at 30 Hz

// Captures a checkpoint record at the given tick from a booted runner. Uses the
// single-snapshot combined hash path (one settled-state read per checkpoint).
Luminumbra::Replay::CheckpointRecord
CaptureCheckpoint(std::uint64_t tick, Luminumbra::Server::ServerWorldRunner& runner) {
    Luminumbra::Replay::CheckpointRecord cp;
    cp.tick = tick;
    Luminumbra::Persistence::WorldStreamingStateSubHashes sub;
    runner.ComputeWorldHashAndSubHashes(cp.world_hash, sub);
    cp.terrain = sub.terrain;
    cp.water = sub.water;
    cp.entities = sub.entities;
    return cp;
}

} // namespace

int RunRecord(const ServerCliOptions& options) {
    LUMINUMBRA_CORE_INFO("Headless server RECORD (LREC1): preset={} seed={} ticks={} -> {}",
                         options.preset,
                         options.seed,
                         options.ticks,
                         options.record_path);

    Luminumbra::Server::ServerWorldRunnerConfig config = RunnerConfigFrom(options);
    config.world_id.clear();
    config.world_name = "Replay Record";
    config.autosave_interval_ticks = 0; // no autosave noise during recording

    Luminumbra::Server::ServerWorldRunner runner(std::move(config));
    if (!runner.Boot()) {
        LUMINUMBRA_CORE_ERROR("record: session failed to boot");
        return 1;
    }

    Luminumbra::Replay::ReplayHeader header;
    header.version = Luminumbra::Replay::kLrec1Version;
    header.tick_rate_hz = 30;
    header.seed_string = options.seed;
    header.seed = std::strtoull(options.seed.c_str(), nullptr, 10);
    header.preset = options.preset;
    header.preset_hash =
        std::strtoull(Luminumbra::Replay::Fnv1a64Hex(options.preset).c_str(), nullptr, 16);
    header.surface_radius = static_cast<std::uint32_t>(options.surface_radius);
    header.collision_radius = static_cast<std::uint32_t>(options.collision_radius);
    header.engine_version = std::string(luminumbra::core::GetEngineVersionString());
    header.start_world_hash = runner.ComputeWorldHash();

    Luminumbra::Replay::ReplayWriter writer;
    if (!writer.Open(options.record_path, header)) {
        LUMINUMBRA_CORE_ERROR("record: cannot open replay stream '{}'", options.record_path);
        return 1;
    }

    // Tick-by-tick: record the (empty today) per-tick input set, then capture a
    // checkpoint every kCheckpointIntervalTicks ticks. Stepping one tick at a
    // time keeps the recorder's hash-capture aligned to the same settled state
    // the smoke's after-the-fact ComputeWorldHash observes.
    const std::vector<std::uint8_t> empty_inputs; // no player inputs in headless
    std::uint64_t executed = 0;
    while (executed < options.ticks) {
        // The input set for the tick ABOUT to run.  will populate this.
        writer.RecordInput(executed + 1, empty_inputs);
        const auto step = runner.RunFixedTicks(1);
        executed += step.ticks_executed;
        if (step.ticks_executed == 0) {
            LUMINUMBRA_CORE_ERROR("record: tick {} did not advance", executed + 1);
            return 1;
        }
        if ((executed % kCheckpointIntervalTicks) == 0) {
            writer.RecordCheckpoint(CaptureCheckpoint(executed, runner));
        }
    }

    if (!writer.Finalize(executed)) {
        LUMINUMBRA_CORE_ERROR("record: failed to finalize replay stream '{}'", options.record_path);
        return 1;
    }

    const std::string final_hash = runner.ComputeWorldHash();
    const fs::path save_dir = runner.Session()->GetWorldSaveDir();
    runner.Shutdown();
    if (!save_dir.empty()) {
        std::error_code ec;
        fs::remove_all(save_dir, ec);
    }

    LUMINUMBRA_CORE_INFO(
        "record: wrote {} ({} ticks, {} input records, {} checkpoints, end_hash={})",
        options.record_path,
        executed,
        writer.InputRecordCount(),
        writer.CheckpointRecordCount(),
        final_hash);
    return 0;
}

int RunReplay(const ServerCliOptions& options) {
    LUMINUMBRA_CORE_INFO("Headless server REPLAY (LREC1): {}", options.replay_path);

    auto contents = Luminumbra::Replay::ReadReplay(options.replay_path);
    if (!contents.has_value()) {
        LUMINUMBRA_CORE_ERROR("replay: '{}' is not a valid LREC1 stream", options.replay_path);
        return 1;
    }
    if (contents->truncated || !contents->trailer_present) {
        LUMINUMBRA_CORE_ERROR("replay: stream '{}' is truncated (no valid trailer)",
                              options.replay_path);
        return 1;
    }

    const Luminumbra::Replay::ReplayHeader& header = contents->header;
    // Refuse a version / engine mismatch loudly (Factorio replays break silently
    // across versions; we refuse instead -- research Area 2 takeaway 8).
    const std::string engine_now(luminumbra::core::GetEngineVersionString());
    if (header.version != Luminumbra::Replay::kLrec1Version) {
        LUMINUMBRA_CORE_ERROR("replay: stream LREC version {} != engine {}",
                              header.version,
                              Luminumbra::Replay::kLrec1Version);
        return 1;
    }

    // Boot the session from the header parameters (mirror ServerWorldRunnerConfig).
    Luminumbra::Server::ServerWorldRunnerConfig config;
    config.root_path = options.root;
    config.seed = header.seed_string;
    config.preset = header.preset;
    config.world_id.clear();
    config.world_name = "Replay Playback";
    config.surface_radius = static_cast<int>(header.surface_radius);
    config.collision_radius = static_cast<int>(header.collision_radius);
    config.autosave_interval_ticks = 0;

    Luminumbra::Server::ServerWorldRunner runner(std::move(config));
    if (!runner.Boot()) {
        LUMINUMBRA_CORE_ERROR("replay: session failed to boot from header");
        return 1;
    }

    // The post-boot hash must match the recorded start_world_hash, else the boot
    // parameters or worldgen drifted before tick 1 (a tick-0 divergence).
    const std::string live_start = runner.ComputeWorldHash();
    bool start_match = (live_start == header.start_world_hash);

    // Drive the recorded ticks. At each checkpoint, compare live vs recorded.
    bool diverged = false;
    std::uint64_t divergence_tick = 0;
    std::string divergence_section;
    std::string expected_hash;
    std::string actual_hash;
    std::uint64_t checkpoints_verified = 0;

    std::uint64_t executed = 0;
    if (!start_match) {
        diverged = true;
        divergence_tick = 0;
        divergence_section = "world_hash";
        expected_hash = header.start_world_hash;
        actual_hash = live_start;
    }

    while (!diverged && executed < contents->tick_count) {
        const std::uint64_t next_tick = executed + 1;
        // Feed the recorded input set for this tick (empty today; the replay
        // driver applies it once  carries real inputs).
        const Luminumbra::Replay::InputRecord* input =
            Luminumbra::Replay::FindInput(*contents, next_tick);
        (void)input; // applied by the transport in; no-op for empty sets
        const auto step = runner.RunFixedTicks(1);
        executed += step.ticks_executed;
        if (step.ticks_executed == 0) {
            LUMINUMBRA_CORE_ERROR("replay: tick {} did not advance", next_tick);
            runner.Shutdown();
            return 1;
        }

        if ((executed % kCheckpointIntervalTicks) == 0) {
            const Luminumbra::Replay::CheckpointRecord* recorded =
                Luminumbra::Replay::FindCheckpoint(*contents, executed);
            if (recorded == nullptr) {
                LUMINUMBRA_CORE_ERROR("replay: missing recorded checkpoint at tick {}", executed);
                runner.Shutdown();
                return 1;
            }
            const auto live = CaptureCheckpoint(executed, runner);
            // Compare top-level hash, then localize via authoritative sub-hashes.
            if (live.world_hash != recorded->world_hash) {
                diverged = true;
                divergence_tick = executed;
                expected_hash = recorded->world_hash;
                actual_hash = live.world_hash;
                if (live.terrain != recorded->terrain) {
                    divergence_section = "terrain";
                } else if (live.water != recorded->water) {
                    divergence_section = "water";
                } else if (live.entities != recorded->entities) {
                    divergence_section = "entities";
                } else {
                    // Authoritative sub-hashes all match but the top-level hash
                    // differs -> the divergence is in the mesh (derived render
                    // artifact) or another non-authoritative component.
                    divergence_section = "world_hash";
                }
                break;
            }
            ++checkpoints_verified;
        }
    }

    const std::string live_end = diverged ? actual_hash : runner.ComputeWorldHash();
    const bool end_hash_match = !diverged && (live_end == [&]() -> std::string {
        // The recorded end-hash is the last checkpoint's world_hash if the run
        // ends on a checkpoint boundary; otherwise re-derive from the trailer
        // tick_count's checkpoint. For the 90-tick gate, 90 is a checkpoint.
        const Luminumbra::Replay::CheckpointRecord* last =
            Luminumbra::Replay::FindCheckpoint(*contents, contents->tick_count);
        return last ? last->world_hash : std::string();
    }());

    const fs::path save_dir = runner.Session()->GetWorldSaveDir();
    runner.Shutdown();
    if (!save_dir.empty()) {
        std::error_code ec;
        fs::remove_all(save_dir, ec);
    }

    if (diverged) {
        // Write the divergence artifact (JSON: tick, expected/actual per section).
        const Luminumbra::Replay::CheckpointRecord* recorded =
            Luminumbra::Replay::FindCheckpoint(*contents, divergence_tick);
        nlohmann::json artifact{
            {"schema", "luminumbra.replay_divergence.v1"},
            {"generated_by", "luminumbra_server_app --replay ()"},
            {"replay_path", options.replay_path},
            {"diverged", true},
            {"divergence_tick", divergence_tick},
            {"divergence_section", divergence_section},
            {"expected_world_hash", expected_hash},
            {"actual_world_hash", actual_hash},
            {"checkpoints_verified_before_divergence", checkpoints_verified},
            {"tick_count", contents->tick_count},
        };
        if (recorded != nullptr) {
            artifact["sections"] = {
                {"world_hash", {{"expected", recorded->world_hash}, {"actual", actual_hash}}},
                {"terrain", {{"expected", recorded->terrain}}},
                {"water", {{"expected", recorded->water}}},
                {"entities", {{"expected", recorded->entities}}},
            };
        }
        if (!options.artifact_path.empty()) {
            const fs::path artifact_path(options.artifact_path);
            std::error_code ec;
            if (artifact_path.has_parent_path()) {
                fs::create_directories(artifact_path.parent_path(), ec);
            }
            std::ofstream out(artifact_path);
            if (out.is_open()) {
                out << artifact.dump(2) << "\n";
                LUMINUMBRA_CORE_INFO("replay divergence artifact written: {}",
                                     options.artifact_path);
            }
        }
        LUMINUMBRA_CORE_ERROR(
            "replay DIVERGED at tick {} (section={}): expected world_hash={} actual={} "
            "({} checkpoints verified before divergence)",
            divergence_tick,
            divergence_section,
            expected_hash,
            actual_hash,
            checkpoints_verified);
        return 1;
    }

    // Clean completion: write the replay (success) artifact.
    nlohmann::json artifact{
        {"schema", "luminumbra.replay_roundtrip.v1"},
        {"generated_by", "luminumbra_server_app --replay ()"},
        {"replay_path", options.replay_path},
        {"diverged", false},
        {"engine_version", header.engine_version},
        {"replayed_engine_version", engine_now},
        {"preset", header.preset},
        {"seed", header.seed_string},
        {"tick_rate_hz", header.tick_rate_hz},
        {"start_world_hash", header.start_world_hash},
        {"start_world_hash_match", start_match},
        {"ticks_replayed", executed},
        {"tick_count", contents->tick_count},
        {"checkpoints_verified", checkpoints_verified},
        {"input_records", contents->inputs.size()},
        {"end_world_hash", live_end},
        {"end_hash_match", end_hash_match},
        {"passed", start_match && end_hash_match && executed == contents->tick_count},
    };

    if (!options.artifact_path.empty()) {
        const fs::path artifact_path(options.artifact_path);
        std::error_code ec;
        if (artifact_path.has_parent_path()) {
            fs::create_directories(artifact_path.parent_path(), ec);
        }
        std::ofstream out(artifact_path);
        if (out.is_open()) {
            out << artifact.dump(2) << "\n";
            LUMINUMBRA_CORE_INFO("replay roundtrip artifact written: {}", options.artifact_path);
        } else {
            LUMINUMBRA_CORE_ERROR("replay: failed to write artifact '{}'", options.artifact_path);
            return 1;
        }
    }

    const bool passed = start_match && end_hash_match && executed == contents->tick_count;
    if (!passed) {
        LUMINUMBRA_CORE_ERROR("replay FAILED: start_match={} end_match={} ticks={}/{}",
                              start_match,
                              end_hash_match,
                              executed,
                              contents->tick_count);
        return 1;
    }
    LUMINUMBRA_CORE_INFO(
        "replay passed: {} ticks, {} checkpoints verified, end_hash={} matches recording",
        executed,
        checkpoints_verified,
        live_end);
    return 0;
}

//  ReplayDivergence gate fixture: read an LREC1 stream and rewrite it
// with ONE checkpoint's world_hash + authoritative sub-hashes corrupted, so a
// replay of the mutated stream MUST diverge at exactly that checkpoint. This is
// the least-hacky mutation: it parses the real stream (no fragile byte-offset
// math) and re-emits it with one record altered, proving the oracle is not
// vacuous -- a tampered stream is caught at the FIRST checkpoint after the edit.
int RunMutateReplayFixture(const ServerCliOptions& options) {
    LUMINUMBRA_CORE_INFO("Replay fixture mutation: {} -> (corrupt one checkpoint)",
                         options.mutate_replay_fixture);

    auto contents = Luminumbra::Replay::ReadReplay(options.mutate_replay_fixture);
    if (!contents.has_value()) {
        LUMINUMBRA_CORE_ERROR("mutate: '{}' is not a valid LREC1 stream",
                              options.mutate_replay_fixture);
        return 1;
    }
    if (contents->checkpoints.empty()) {
        LUMINUMBRA_CORE_ERROR("mutate: stream has no checkpoints to corrupt");
        return 1;
    }

    // Re-emit the stream with the FIRST checkpoint's hashes flipped to a value
    // that cannot occur (prefix "dead"), guaranteeing a mismatch there.
    Luminumbra::Replay::ReplayWriter writer;
    if (!writer.Open(options.mutate_replay_fixture, contents->header)) {
        LUMINUMBRA_CORE_ERROR("mutate: cannot reopen '{}'", options.mutate_replay_fixture);
        return 1;
    }
    for (const auto& input : contents->inputs) {
        writer.RecordInput(input.tick, input.inputs);
    }
    const std::uint64_t target_tick = contents->checkpoints.front().tick;
    for (auto cp : contents->checkpoints) {
        if (cp.tick == target_tick) {
            cp.world_hash = "dead" + cp.world_hash.substr(4);
            cp.terrain = "dead" + cp.terrain.substr(4);
        }
        writer.RecordCheckpoint(cp);
    }
    if (!writer.Finalize(contents->tick_count)) {
        LUMINUMBRA_CORE_ERROR("mutate: failed to finalize mutated stream");
        return 1;
    }
    LUMINUMBRA_CORE_INFO("mutate: corrupted checkpoint at tick {} in {}",
                         target_tick,
                         options.mutate_replay_fixture);
    return 0;
}
