#include "core/ScenarioRunner.h"

namespace Luminumbra::Client::ScenarioHarness {

ScenarioRunnerFactory GetScenarioRunnerFactory() {
    return &CreateScenarioRunner;
}

} // namespace Luminumbra::Client::ScenarioHarness
