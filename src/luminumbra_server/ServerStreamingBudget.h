#pragma once

#include <chrono>
#include <cstdint>

#include "luminumbra_common/simulation/SimBudgetTelemetry.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"

namespace Luminumbra::Server {

// Observation only. The clock parameter lets the regression exercise complete
// update boundaries without sleeps or depending on machine speed.
template<typename Clock = std::chrono::steady_clock>
class ServerStreamingBudget {
public:
    explicit ServerStreamingBudget(luminumbra::simulation::SimBudgetTelemetry& budget)
        : budget_(budget) {}

    template<typename Update>
    void MeasureUpdate(Update&& update) {
        if (!budget_.Enabled()) {
            update();
            return;
        }
        const auto start = Clock::now();
        update();
        update_ms_ = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    }

    void Record(std::uint64_t tick,
                const Systems::SHIELD_WorldSystem::DbgStreamTimings& timing,
                std::uint64_t scheduled,
                std::uint64_t water_work,
                double activation_wait_ms) {
        using luminumbra::simulation::SimBudgetStage;
        // Water is contained in the update; the due-activation wait is disjoint.
        budget_.Record(SimBudgetStage::ServerStreaming,
                       tick,
                       scheduled,
                       update_ms_ - timing.water + activation_wait_ms);
        budget_.Record(SimBudgetStage::ServerWater, tick, water_work, timing.water);
    }

    void RecordWorkItems(std::uint64_t tick,
                         const Systems::SHIELD_WorldSystem::StreamingBudgetFrameStats& streaming,
                         Systems::PhysicsSystem::WorkItems physics,
                         std::uint64_t water_cells,
                         std::uint64_t snapshot_copies) {
        using luminumbra::simulation::SimWork;
        budget_.RecordWork(SimWork::WaterCells, tick, water_cells);
        budget_.RecordWork(SimWork::StreamingGeneration, tick, streaming.scheduled_generation);
        budget_.RecordWork(SimWork::StreamingMeshing, tick, streaming.scheduled_meshing);
        budget_.RecordWork(SimWork::PhysicsBodies, tick, physics.bodies);
        budget_.RecordWork(SimWork::PhysicsCharacters, tick, physics.characters);
        budget_.RecordWork(SimWork::SnapshotCopies, tick, snapshot_copies);
        budget_.RecordWork(SimWork::ActivationQueries, tick, streaming.target_surface_columns);
    }

private:
    luminumbra::simulation::SimBudgetTelemetry& budget_;
    double update_ms_ = 0.0;
};

} // namespace Luminumbra::Server
