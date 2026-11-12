#pragma once

#include "persistence/PlantSnapshot.h"
#include "persistence/SaveTransaction.h"
#include "persistence/WorldSaveService.h"
#include "world/ActiveRegionLedger.h"
#include "world/GroundObjectStore.h"
#include "world/WorldClock.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Luminumbra::Test {

// Publish a valid replacement from an admitted baseline. Corruption is injected only after
// this function returns. Writers join one transaction; absent members remain explicit tombstones.
template<typename Write>
bool PublishIndexedSave(const std::filesystem::path& directory,
                        const Write& write,
                        std::vector<std::string>* errors = nullptr) {
    {
        const Persistence::WorldSaveService::SaveOwnership ownership;
        if (Persistence::recover_pending_journal(directory, errors) ==
            Persistence::RecoveryOutcome::Failed)
            return false;
        Persistence::WorldSaveService::SaveFactsScope facts(directory);
        if (!facts.validate(errors))
            return false;
        Persistence::SnapshotIndex previous;
        if (!Persistence::read_snapshot_index(directory, previous, errors))
            return false;
        Persistence::SaveTransaction transaction(directory,
                                                 previous.tick,
                                                 previous.anchors,
                                                 &Persistence::WorldSaveService::member_digest);
        if (!transaction.begin(errors) ||
            !transaction.protect({"world_info.json",
                                  "aether_state.efs",
                                  "chunks/region/active-regions.arl",
                                  "chunks/region/world-manifest.json",
                                  "chunks/region/plant-entities.json"},
                                 errors) ||
            !write() ||
            !Persistence::WorldSaveService::finalize_indexed_envelope(directory, errors) ||
            !transaction.commit(errors))
            return false;
        facts.run_deferred();
    }
    return Persistence::WorldSaveService::validate_save(directory, errors);
}

// Prepare the entire candidate before any transaction artifacts or payload writes.
// A null regional ledger models regions disabled: the mandatory envelope owns its cursor.
inline bool PublishPlantSnapshotSave(const std::filesystem::path& directory,
                                     const Persistence::PlantSnapshot& snapshot,
                                     const world::ActiveRegionLedger* regional_ledger = nullptr,
                                     std::vector<std::string>* errors = nullptr,
                                     std::uint64_t tick = 0) {
    using Service = Persistence::WorldSaveService;
    const Persistence::PlantSnapshotContract contract;
    Service::PreparedPlantSnapshotWrite prepared;
    world::ActiveRegionLedger detached;
    std::string error;
    if (!prepared.prepare(snapshot, errors) ||
        (!regional_ledger && !detached.reserve_persistent_ids(snapshot.next_id)))
        return false;
    const auto& ledger = regional_ledger ? *regional_ledger : detached;
    if (!Persistence::ValidatePlantSnapshot(snapshot, ledger, world::GroundObjectStore{}, error)) {
        if (errors)
            errors->push_back(error);
        return false;
    }
    const auto ledger_bytes = regional_ledger ? regional_ledger->encode() : std::string{};
    nlohmann::json metadata = {{"container_version", Service::kContainerVersion}};
    world::WorldClock(std::max(tick, ledger.header_tick())).write_metadata(metadata);
    const auto metadata_bytes = metadata.dump();
    return PublishIndexedSave(
        directory,
        [&] {
            auto* transaction = Persistence::SaveTransaction::active_for(directory);
            if (!transaction)
                return false;
            if (regional_ledger) {
                if (!Service::write_member_bytes(
                        directory, "chunks/region/active-regions.arl", ledger_bytes, errors))
                    return false;
            } else if (!Service::remove_member(
                           directory, "chunks/region/active-regions.arl", errors)) {
                return false;
            }
            return transaction->publish_plant_snapshot(prepared.bytes(), errors) &&
                   Service::write_member_bytes(
                       directory, "world_info.json", metadata_bytes, errors);
        },
        errors);
}

} // namespace Luminumbra::Test
