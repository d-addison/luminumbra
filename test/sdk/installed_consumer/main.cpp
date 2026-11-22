#include <luminumbra/abi/luminumbra_abi.h>
#include <luminumbra/modules/ModuleRegistry.h>
#include <luminumbra/modules/PhaseScheduler.h>

#include <type_traits>

extern "C" int luminumbra_sdk_abi_valid(void);

static_assert(std::is_standard_layout_v<LumHostApi>);
static_assert(Luminumbra::ModuleRegistry::MaxModules == Luminumbra::PhaseScheduler::MaxTasks);
static_assert(!std::is_copy_constructible_v<Luminumbra::ModuleRegistry>);

int main() {
    // The installed target supplies declarations; this consumer owns its data.
    const Luminumbra::ModuleDescriptor module{
        "consumer", {"consumer.service"}, {}, nullptr, nullptr, nullptr};
    const Luminumbra::PhaseTask task{"consumer.task", "Startup", {}};
    const Luminumbra::ExecutionTrace trace;
    return luminumbra_sdk_abi_valid() != 0 && module.provides.size() == 1 &&
                   task.phase == "Startup" && trace.hash() == 14695981039346656037ULL
               ? 0
               : 1;
}
