#pragma once

#include "persistence/SaveTransaction.h"
#include "persistence/WorldSaveService.h"

#include <filesystem>
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

} // namespace Luminumbra::Test
