#include "WorldSaveService.h"

#include "WorldPersistenceRoundtrip.h"

#include <fstream>
#include <sstream>
#include <system_error>

namespace Luminumbra::Persistence {
namespace {

constexpr const char* kChunksDirectoryName = "chunks";
constexpr const char* kWorldStateFileName = "world-state.json";

} // namespace

std::filesystem::path WorldSaveService::world_state_path(const std::filesystem::path& save_dir) {
    return save_dir / kChunksDirectoryName / kWorldStateFileName;
}

bool WorldSaveService::save_world(
    const WorldStreamingState& state,
    const std::filesystem::path& save_dir,
    std::vector<std::string>* errors) const {
    return write_snapshot(SerializeWorldStreamingStateSnapshotJson(state), save_dir, errors);
}

bool WorldSaveService::load_world(
    WorldStreamingState& state,
    const std::filesystem::path& save_dir,
    std::vector<std::string>& errors) const {
    const std::filesystem::path snapshot_path = world_state_path(save_dir);

    std::error_code exists_error;
    if (!std::filesystem::exists(snapshot_path, exists_error) || exists_error) {
        // Fresh world: nothing has been persisted yet. Not an error.
        return false;
    }

    std::ifstream input(snapshot_path, std::ios::binary);
    if (!input.is_open()) {
        errors.push_back("failed to open world state snapshot for reading: " + snapshot_path.string());
        return false;
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad()) {
        errors.push_back("failed to read world state snapshot: " + snapshot_path.string());
        return false;
    }

    return LoadWorldStreamingStateSnapshotJson(buffer.str(), state, errors);
}

std::string WorldSaveService::world_hash(const WorldStreamingState& state) const {
    return ComputeWorldStreamingStateHash(state);
}

bool WorldSaveService::write_snapshot(
    const std::string& snapshot_json,
    const std::filesystem::path& save_dir,
    std::vector<std::string>* errors) const {
    try {
        const std::filesystem::path snapshot_path = world_state_path(save_dir);
        const std::filesystem::path parent = snapshot_path.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent);
        }

        std::ofstream output(snapshot_path, std::ios::binary | std::ios::trunc);
        if (!output.is_open()) {
            if (errors) {
                errors->push_back("failed to open world state snapshot for writing: " + snapshot_path.string());
            }
            return false;
        }

        output << snapshot_json;
        output.flush();
        if (!output.good()) {
            if (errors) {
                errors->push_back("failed to write world state snapshot: " + snapshot_path.string());
            }
            return false;
        }
        return true;
    } catch (const std::exception& e) {
        if (errors) {
            errors->push_back(std::string("failed to write world state snapshot: ") + e.what());
        }
        return false;
    }
}

} // namespace Luminumbra::Persistence
