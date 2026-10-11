#include "ModeHelpers.h"
#include "Modes.h"

namespace fs = std::filesystem;

int RunServer(const ServerCliOptions& options) {
    Luminumbra::Server::ServerWorldRunner runner(RunnerConfigFrom(options));
    if (!runner.Boot()) {
        return 1;
    }

    const Luminumbra::Server::ServerTickReport report = runner.RunFixedTicks(options.ticks);
    LUMINUMBRA_CORE_INFO("Headless server run complete: {} ticks ({:.2f}s simulated) in {:.2f}s "
                         "wall, {} autosave passes",
                         report.ticks_executed,
                         report.simulated_seconds,
                         report.wall_seconds,
                         report.autosave_passes);

    runner.Shutdown();
    if (report.region_fault) {
        LUMINUMBRA_CORE_ERROR("Headless server stopped by a region scheduler fault: {}",
                              report.region_fault_message);
        return 3;
    }
    return report.ticks_executed == options.ticks ? 0 : 1;
}
