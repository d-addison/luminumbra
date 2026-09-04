#include "persistence/WorldPersistenceRoundtrip.h"

#include "../support/EntitySnapshotFixture.h"

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    const std::filesystem::path output_path =
        argc > 1 ? std::filesystem::path(argv[1])
                 : std::filesystem::path(
                       "build/debug/test-artifacts/persistence/world-persistence-roundtrip.json");
    const std::string build_preset = argc > 2 ? argv[2] : "debug";
    const std::filesystem::path chunk_format_output_path =
        argc > 3 ? std::filesystem::path(argv[3])
                 : output_path.parent_path() / "chunk-format-validation.json";
    const std::filesystem::path world_hash_output_path =
        argc > 4 ? std::filesystem::path(argv[4]) : output_path.parent_path() / "world-hash.json";
    const std::filesystem::path entity_snapshot_output_path =
        argc > 5 ? std::filesystem::path(argv[5])
                 : output_path.parent_path() / "entity-snapshot.json";

    std::vector<std::string> errors;
    if (!Luminumbra::Persistence::WriteWorldPersistenceRoundtripArtifact(
            output_path, build_preset, &errors)) {
        for (const std::string& error : errors) {
            std::cerr << error << '\n';
        }
        return 1;
    }
    if (!Luminumbra::Persistence::WriteChunkFormatValidationArtifact(
            chunk_format_output_path, build_preset, &errors)) {
        for (const std::string& error : errors) {
            std::cerr << error << '\n';
        }
        return 1;
    }
    errors.clear();
    if (!Luminumbra::Persistence::WriteWorldHashArtifact(
            world_hash_output_path, build_preset, &errors)) {
        for (const std::string& error : errors) {
            std::cerr << error << '\n';
        }
        return 1;
    }
    errors.clear();
    if (!Luminumbra::Persistence::WriteEntitySnapshotArtifact(
            entity_snapshot_output_path,
            build_preset,
            Luminumbra::TestSupport::BuildEntitySnapshotFixture(),
            &errors)) {
        for (const std::string& error : errors) {
            std::cerr << error << '\n';
        }
        return 1;
    }

    return 0;
}
