#pragma once

#include <filesystem>

namespace Luminumbra::Client::ScenarioHarness {

// GL-free asset producer shared by the visual scenario and its CPU regression.
// The legacy smoke rig deliberately retains LANM1 bytes and quaternion nlerp.
// Parent directories must exist; false reports output stream failures.
bool WriteSkinnedTestAssets(const std::filesystem::path& mesh_path,
                            const std::filesystem::path& clip_path);

} // namespace Luminumbra::Client::ScenarioHarness
