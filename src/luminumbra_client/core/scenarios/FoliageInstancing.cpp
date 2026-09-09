#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "core/RuntimeScenarioHarness.h"
#include "core/scenarios/ScenarioCommon.h"

#include <fstream>
#include <iomanip>

namespace Luminumbra::Client::ScenarioHarness {

void WriteFoliageInstancingAnalysis(
    const std::filesystem::path& artifact_dir,
    const std::string& foliage_screenshot,
    const FoliageInstancingResult& result,
    const Luminumbra::Rendering::RenderPipeline::RenderPassFrameStats& render_pass) {
    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();
    nlohmann::json artifact = BuildFoliageInstancingReport(result, gl_debug.errors);
    artifact["timestamp_utc"] = TimestampUtc();
    artifact["foliage_screenshot"] = foliage_screenshot;
    artifact["render_pass"]["skybox_draws"] = render_pass.skybox_draws;
    artifact["gl_debug"] = {{"messages", gl_debug.messages},
                            {"errors", gl_debug.errors},
                            {"warnings", gl_debug.warnings},
                            {"notifications", gl_debug.notifications}};
    std::ofstream output(artifact_dir / "foliage-instancing-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
