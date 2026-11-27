#pragma once

#include "luminumbra_common/core/Log.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace Luminumbra::Server {

// Consume an output file using its default, nonthrowing stream exception mask.
// Close even after a short write, and include final buffered writes in the verdict.
[[nodiscard]] inline bool WriteJsonArtifact(std::ofstream& output,
                                            const nlohmann::json& artifact,
                                            const std::string& artifact_path) {
    output << artifact.dump(2) << "\n";
    output.close();
    if (!output) {
        LUMINUMBRA_CORE_ERROR("Failed to finish JSON artifact: {}", artifact_path);
        return false;
    }
    return true;
}

} // namespace Luminumbra::Server
