#pragma once

// Diagnostic session driver only; never include from src/.
#include "GameSessionTestWorld.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/JobSystem.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <utility>

namespace Luminumbra::TestSupport {

class RegionalFieldEvidence {
public:
    explicit RegionalFieldEvidence(bool diagnostic_pressure)
        : pressure_(diagnostic_pressure) {
        jobs_.startup(1);
    }
    ~RegionalFieldEvidence() {
        jobs_.shutdown();
    }
    RegionalFieldEvidence(const RegionalFieldEvidence&) = delete;
    RegionalFieldEvidence& operator=(const RegionalFieldEvidence&) = delete;

    // Axial route, at most 20,000 m, 160 stops and 640 recorded ticks. Captures
    // scan at most 256 owners and 1024 physical pages per tick, outside tick timing.
    nlohmann::json Capture(std::uint32_t distance, std::uint32_t stride) {
        if (distance == 0 || distance > 20000 || stride < 128 || stride > 512)
            throw std::invalid_argument("invalid field evidence route");
        world::GameSession source, loaded;
        Configure(source);
        if (!source.CreateWorld("Regional field evidence", "1337", "default") ||
            !source.LoadWorldStateFrom(root_.path() / "unmarked-snapshot"))
            throw std::runtime_error("cannot create field evidence world");
        source.GetWorldSystem()->SetWaterHydrology(false, 0, 0);
        // Leave the replicated roster absent: an explicit empty roster suppresses
        // the local walking anchor, so no ambient owners or completed work are admitted.
        if (!source.GetFieldPages().empty())
            throw std::runtime_error("field evidence requires empty initial authority");
        nlohmann::json frames = nlohmann::json::array();
        std::map<world::RegionKey, std::string> previous;
        std::uint32_t travelled = 0;
        for (;;) {
            source.SetLocalPlayerSimulationPosition(
                Vec3(256 + static_cast<float>(travelled), 0, 256), true);
            for (unsigned dwell = 0; dwell < 4; ++dwell) {
                const auto elapsed = Step(source);
                frames.push_back(Frame(source, travelled, elapsed, previous));
            }
            if (travelled == distance)
                break;
            travelled += std::min(stride, distance - travelled);
        }
        const auto save_start = Clock::now();
        if (!source.SaveWorld())
            throw std::runtime_error("cannot save field evidence world: " +
                                     source.GetWorldOpenError());
        const auto save_us = Elapsed(save_start);
        const auto fields = source.GetFieldPages().serialize();
        const auto ledger = source.GetActiveRegionLedger().canonical_bytes();
        Configure(loaded);
        const auto load_start = Clock::now();
        if (!loaded.LoadWorld(source.GetMetadata().worldId))
            throw std::runtime_error("cannot reopen field evidence world: " +
                                     loaded.GetWorldOpenError());
        const auto load_us = Elapsed(load_start);
        RequireEqual(source, loaded);
        if (loaded.GetSimulationTickCount() != source.GetSimulationTickCount())
            throw std::runtime_error("reopen advanced field evidence clock");
        // Reapply only public host inputs; load itself does no catch-up work.
        // Reopen also leaves the replicated roster absent, matching the source.
        loaded.SetLocalPlayerSimulationPosition(Vec3(256 + static_cast<float>(distance), 0, 256),
                                                true);
        for (unsigned continuation = 0; continuation < 2; ++continuation) {
            Step(source);
            Step(loaded);
            RequireEqual(source, loaded);
        }
        const auto& config = source.GetActiveRegionLedger().config();
        return {{"schema", "luminumbra.field_page_evidence.v1"},
                {"profile", pressure_ ? "diagnostic-pressure" : "shipped-settings"},
                {"scheduler",
                 {{"work_limit", config.work_limit},
                  {"hold_ticks", config.hold_ticks},
                  {"reduced_shift", config.reduced_shift}}},
                {"seed", "1337"},
                {"distance_m", distance},
                {"stride_m", stride},
                {"frames", std::move(frames)},
                {"reopen",
                 {{"tick", source.GetSimulationTickCount() - 2},
                  {"fields_digest", Persistence::StableChecksum(fields)},
                  {"ledger_digest", Persistence::StableChecksum(ledger)},
                  {"exact_equal", true},
                  {"continuation_ticks", 2},
                  {"save_us", save_us},
                  {"load_us", load_us}}}};
    }

private:
    using Clock = std::chrono::steady_clock;
    static std::int64_t Elapsed(Clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - start).count();
    }
    void Configure(world::GameSession& session) {
        session.SetRootPath(Filesystem::GenericPathString(root_.path()) + '/');
        session.SetJobSystem(&jobs_);
        session.SetActiveRegionsEnabled(true);
        if (pressure_)
            session.SetRegionSchedulerConfig({10, 2, 1});
        session.SetAetherStateEnabled(false);
        session.SetWaterHighResEnabled(false);
        session.SetWeatherRainEnabled(false);
        session.SetWeatherEventsEnabled(false);
        session.SetFiniteHydrologyEnabled(false);
    }
    static std::int64_t Step(world::GameSession& session) {
        const auto start = Clock::now();
        const auto ticks = session.TickSimulation(session.GetSimulationClock().fixed_dt());
        const auto elapsed = Elapsed(start);
        if (ticks != 1 || session.GetRegionSchedulerFault())
            throw std::runtime_error("field evidence simulation tick refused");
        return elapsed;
    }
    static void RequireEqual(const world::GameSession& source, const world::GameSession& loaded) {
        if (source.GetFieldPages().serialize() != loaded.GetFieldPages().serialize() ||
            source.GetActiveRegionLedger().encode() != loaded.GetActiveRegionLedger().encode())
            throw std::runtime_error("field evidence reopen authority mismatch");
    }
    static nlohmann::json Frame(const world::GameSession& session,
                                std::uint32_t travelled,
                                std::int64_t elapsed,
                                std::map<world::RegionKey, std::string>& previous) {
        const auto& ledger = session.GetActiveRegionLedger();
        const auto& fields = session.GetFieldPages();
        const auto& anchor = ledger.local_anchor();
        if (!anchor || *anchor != Vec3(256 + static_cast<float>(travelled), 0, 256) ||
            session.GetRegionSchedule().tick != session.GetSimulationTickCount())
            throw std::runtime_error("field evidence anchor or schedule tick mismatch");
        if (ledger.records().size() > 256 || fields.page_count() > 1024)
            throw std::runtime_error("field evidence capture capacity exceeded");
        nlohmann::json owners = nlohmann::json::array();
        for (const auto& [key, record] : ledger.records()) {
            auto bytes = fields.region_bytes(key);
            const auto prior = previous.find(key);
            const bool activated =
                !bytes.empty() && (prior == previous.end() || prior->second.empty());
            const bool frozen = record.state == world::RegionState::Frozen;
            if (frozen && (bytes.empty() || prior == previous.end() || prior->second != bytes))
                throw std::runtime_error("frozen owner bytes changed or were never activated");
            const auto work = ledger.completed_work().find(key);
            owners.push_back(
                {{"key", {key.x, key.z}},
                 {"state", static_cast<unsigned>(record.state)},
                 {"cells", fields.owned_initialized_cells(key)},
                 {"digest", Persistence::StableChecksum(bytes)},
                 {"activated", activated},
                 {"frozen_at", record.frozen_at},
                 {"frozen_digest", record.frozen_digest},
                 {"last_ticked", record.last_ticked},
                 {"completed_work", work == ledger.completed_work().end() ? 0 : work->second}});
            previous.insert_or_assign(key, std::move(bytes));
        }
        nlohmann::json masks = nlohmann::json::array();
        for (const auto& [key, page] : fields.pages())
            masks.push_back({{"key", {key.x, key.z}}, {"words", page.mask}});
        nlohmann::json due = nlohmann::json::array();
        for (const auto& key : session.GetRegionSchedule().due)
            due.push_back({key.x, key.z});
        return {{"tick", session.GetSimulationTickCount()},
                {"anchor",
                 {static_cast<std::int32_t>(anchor->x),
                  static_cast<std::int32_t>(anchor->y),
                  static_cast<std::int32_t>(anchor->z)}},
                {"schedule_digest", session.GetRegionSchedule().digest},
                {"schedule_work", session.GetRegionSchedule().work},
                {"activation_visits", session.GetRegionalFieldActivationVisits()},
                {"due", std::move(due)},
                {"owners", std::move(owners)},
                {"masks", std::move(masks)},
                {"fields_digest", Persistence::StableChecksum(fields.serialize())},
                {"ledger_digest", Persistence::StableChecksum(ledger.canonical_bytes())},
                {"tick_us", elapsed}};
    }
    HeadlessRoot root_;
    JobSystem jobs_;
    bool pressure_;
};

} // namespace Luminumbra::TestSupport
