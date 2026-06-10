#pragma once

#include <string>
#include <vector>

namespace Luminumbra::Client::Debug {

struct RuntimeOverlaySchema {
    std::vector<std::string> chunk_fields;
    std::vector<std::string> queue_fields;
    std::vector<std::string> render_fields;
    std::vector<std::string> performance_fields;
    std::vector<std::string> shader_fields;
    std::vector<std::string> missing_horizon_reasons;
};

RuntimeOverlaySchema BuildRuntimeOverlaySchema();
std::string RuntimeOverlaySchemaName();

} // namespace Luminumbra::Client::Debug
