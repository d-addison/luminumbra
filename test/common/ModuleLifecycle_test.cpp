#include "luminumbra_common/core/ModuleRegistry.h"
#include "luminumbra_common/core/PhaseScheduler.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {

using Luminumbra::ModuleDescriptor;
using Luminumbra::ModuleRegistry;
using Luminumbra::ModuleResult;
using Luminumbra::PhaseScheduler;
using Luminumbra::PhaseTask;
using Luminumbra::ScheduleResult;

// Fixed hook log: no allocation or throwing assertions inside noexcept hooks.
struct HookLog {
    std::array<int, 32> entries{};
    std::size_t count = 0;
    void append(int entry) noexcept {
        if (count < entries.size()) {
            entries[count] = entry;
        }
        ++count;
    }
    std::vector<int> values() const {
        return {entries.begin(),
                entries.begin() + static_cast<std::ptrdiff_t>(std::min(count, entries.size()))};
    }
};

struct HookContext {
    HookLog* log = nullptr;
    int id = 0;
    bool succeeds = true;
};

bool StartHook(void* data) noexcept {
    auto& context = *static_cast<HookContext*>(data);
    context.log->append(context.id);
    return context.succeeds;
}

void StopHook(void* data) noexcept {
    auto& context = *static_cast<HookContext*>(data);
    context.log->append(-context.id);
}

ModuleDescriptor Module(std::string_view id,
                        const std::vector<std::string>& provides = {},
                        const std::vector<std::string>& required = {},
                        HookContext* context = nullptr) {
    return {std::string(id), provides, required, StartHook, StopHook, context};
}

std::vector<std::string> RunPhase(PhaseScheduler& scheduler, std::size_t index) {
    std::vector<std::string> executed;
    EXPECT_EQ(scheduler.execute(index, [&](std::string_view id) { executed.emplace_back(id); }),
              ScheduleResult::Ok);
    return executed;
}

TEST(ModuleLifecycleTest, TopologicalStartupReverseShutdownAndIdempotence) {
    HookLog log;
    std::array<HookContext, 4> contexts{
        {{&log, 1, true}, {&log, 2, true}, {&log, 3, true}, {&log, 4, true}}};
    ModuleRegistry registry;
    ASSERT_EQ(registry.register_module(Module("a", {}, {"world"}, &contexts[0])), ModuleResult::Ok);
    ASSERT_EQ(registry.register_module(Module("z", {"world"}, {}, &contexts[1])), ModuleResult::Ok);
    ASSERT_EQ(registry.register_module(Module("b", {}, {}, &contexts[2])), ModuleResult::Ok);
    ASSERT_EQ(registry.register_module(Module("c", {}, {"world", "world"}, &contexts[3])),
              ModuleResult::Ok);
    ASSERT_EQ(registry.startup(), ModuleResult::Ok);
    EXPECT_TRUE(registry.running());
    EXPECT_EQ(log.values(), (std::vector<int>{3, 2, 1, 4}));
    EXPECT_EQ(registry.startup(), ModuleResult::Ok);
    EXPECT_EQ(log.count, 4U);
    EXPECT_EQ(registry.register_module(Module("extra")), ModuleResult::Busy);
    EXPECT_EQ(registry.shutdown(), ModuleResult::Ok);
    EXPECT_FALSE(registry.running());
    EXPECT_EQ(log.values(), (std::vector<int>{3, 2, 1, 4, -4, -1, -2, -3}));
    const auto stopped_hash = registry.execution_trace_hash();
    EXPECT_EQ(registry.shutdown(), ModuleResult::Ok);
    EXPECT_EQ(registry.execution_trace_hash(), stopped_hash);
    ASSERT_EQ(registry.startup(), ModuleResult::Ok);
    ASSERT_EQ(registry.shutdown(), ModuleResult::Ok);
    EXPECT_EQ(log.count, 16U);
}

TEST(ModuleLifecycleTest, DestructorStopsLiveModulesExactlyOnce) {
    HookLog log;
    HookContext context{&log, 1, true};
    {
        ModuleRegistry registry;
        ASSERT_EQ(registry.register_module(Module("owned", {}, {}, &context)), ModuleResult::Ok);
        ASSERT_EQ(registry.startup(), ModuleResult::Ok);
    }
    EXPECT_EQ(log.values(), (std::vector<int>{1, -1}));
    EXPECT_FALSE(std::is_copy_constructible_v<ModuleRegistry>);
    EXPECT_FALSE(std::is_move_constructible_v<ModuleRegistry>);
}

TEST(ModuleLifecycleTest, FailedStartupUnwindsOnlySuccessfulHooksAndCanRetry) {
    for (std::size_t failed = 0; failed < 3; ++failed) {
        HookLog log;
        std::array<HookContext, 3> contexts{{{&log, 1, true}, {&log, 2, true}, {&log, 3, true}}};
        contexts[failed].succeeds = false;
        ModuleRegistry registry;
        ASSERT_EQ(registry.register_module(Module("a", {"a"}, {}, &contexts[0])), ModuleResult::Ok);
        ASSERT_EQ(registry.register_module(Module("b", {"b"}, {"a"}, &contexts[1])),
                  ModuleResult::Ok);
        ASSERT_EQ(registry.register_module(Module("c", {}, {"b"}, &contexts[2])), ModuleResult::Ok);
        EXPECT_EQ(registry.startup(), ModuleResult::StartupFailed);
        EXPECT_FALSE(registry.running());
        std::vector<int> expected;
        for (std::size_t index = 0; index <= failed; ++index) {
            expected.push_back(static_cast<int>(index + 1));
        }
        for (std::size_t index = failed; index > 0; --index) {
            expected.push_back(-static_cast<int>(index));
        }
        EXPECT_EQ(log.values(), expected);
        const auto hash = registry.execution_trace_hash();
        EXPECT_EQ(registry.shutdown(), ModuleResult::Ok);
        EXPECT_EQ(registry.execution_trace_hash(), hash);
        contexts[failed].succeeds = true;
        ASSERT_EQ(registry.startup(), ModuleResult::Ok);
        ASSERT_EQ(registry.shutdown(), ModuleResult::Ok);
        EXPECT_EQ(log.count, expected.size() + 6);
    }
}

TEST(ModuleLifecycleTest, MissingServiceRefusesBeforeHooksAndAllowsRepair) {
    HookLog log;
    HookContext context{&log, 1, true};
    ModuleRegistry registry;
    ASSERT_EQ(registry.register_module(Module("consumer", {}, {"service"}, &context)),
              ModuleResult::Ok);
    const auto hash = registry.execution_trace_hash();
    EXPECT_EQ(registry.startup(), ModuleResult::MissingService);
    EXPECT_EQ(registry.execution_trace_hash(), hash);
    EXPECT_EQ(log.count, 0U);
    ASSERT_EQ(registry.register_module({"provider", {"service"}, {}, nullptr, nullptr, nullptr}),
              ModuleResult::Ok);
    EXPECT_EQ(registry.startup(), ModuleResult::Ok);
    EXPECT_EQ(registry.shutdown(), ModuleResult::Ok);
    EXPECT_EQ(log.values(), (std::vector<int>{1, -1}));
}

TEST(ModuleLifecycleTest, SelfAndMultiModuleCyclesRefuseBeforeAnyHooks) {
    for (const bool self_dependency : {false, true}) {
        HookLog log;
        HookContext context{&log, 1, true};
        ModuleRegistry registry;
        ASSERT_EQ(
            registry.register_module(Module("a", {"a"}, {self_dependency ? "a" : "b"}, &context)),
            ModuleResult::Ok);
        ASSERT_EQ(registry.register_module(Module("b", {"b"}, {"a"}, &context)), ModuleResult::Ok);
        ASSERT_EQ(registry.register_module(Module("independent", {}, {}, &context)),
                  ModuleResult::Ok);
        const auto hash = registry.execution_trace_hash();
        EXPECT_EQ(registry.startup(), ModuleResult::Cycle);
        EXPECT_EQ(registry.execution_trace_hash(), hash);
        EXPECT_EQ(log.count, 0U);
        EXPECT_FALSE(registry.running());
    }
}

TEST(ModuleLifecycleTest, DuplicateIdsAndAmbiguousProvidersLeaveRegistrationUnchanged) {
    ModuleRegistry registry;
    ASSERT_EQ(registry.register_module({"a", {"one"}, {}, nullptr, nullptr, nullptr}),
              ModuleResult::Ok);
    EXPECT_EQ(registry.register_module(Module("a")), ModuleResult::DuplicateId);
    EXPECT_EQ(registry.register_module(Module("b", {"one"})), ModuleResult::DuplicateService);
    EXPECT_EQ(registry.register_module(Module("b", {"two", "two"})),
              ModuleResult::DuplicateService);
    EXPECT_EQ(registry.size(), 1U);
    EXPECT_EQ(registry.register_module({"b", {"two"}, {}, nullptr, nullptr, nullptr}),
              ModuleResult::Ok);
    EXPECT_EQ(registry.startup(), ModuleResult::Ok);
    EXPECT_EQ(registry.shutdown(), ModuleResult::Ok);
}

TEST(ModuleLifecycleTest, OptionalHooksAndEmptyRegistryAreValid) {
    HookLog log;
    HookContext context{&log, 1, true};
    ModuleRegistry registry;
    const auto empty_hash = registry.execution_trace_hash();
    EXPECT_EQ(registry.startup(), ModuleResult::Ok);
    EXPECT_EQ(registry.shutdown(), ModuleResult::Ok);
    EXPECT_EQ(registry.execution_trace_hash(), empty_hash);
    ASSERT_EQ(registry.register_module({"no_hooks", {}, {}, nullptr, nullptr, nullptr}),
              ModuleResult::Ok);
    ASSERT_EQ(registry.register_module({"only_start", {}, {}, StartHook, nullptr, &context}),
              ModuleResult::Ok);
    ASSERT_EQ(registry.register_module({"only_stop", {}, {}, nullptr, StopHook, &context}),
              ModuleResult::Ok);
    EXPECT_EQ(registry.startup(), ModuleResult::Ok);
    EXPECT_EQ(registry.shutdown(), ModuleResult::Ok);
    EXPECT_EQ(log.values(), (std::vector<int>{1, -1}));
}

struct ReentryContext {
    ModuleRegistry* registry = nullptr;
    std::array<ModuleResult, 6> results{};
};

bool ReentrantStart(void* data) noexcept {
    auto& context = *static_cast<ReentryContext*>(data);
    context.results[0] = context.registry->startup();
    context.results[1] = context.registry->shutdown();
    context.results[2] = context.registry->register_module({});
    return true;
}

void ReentrantStop(void* data) noexcept {
    auto& context = *static_cast<ReentryContext*>(data);
    context.results[3] = context.registry->startup();
    context.results[4] = context.registry->shutdown();
    context.results[5] = context.registry->register_module({});
}

TEST(ModuleLifecycleTest, LifecycleHooksCannotReenterOrMutateTheirRegistry) {
    ReentryContext context;
    ModuleRegistry registry;
    context.registry = &registry;
    ASSERT_EQ(
        registry.register_module({"guarded", {}, {}, ReentrantStart, ReentrantStop, &context}),
        ModuleResult::Ok);
    ASSERT_EQ(registry.startup(), ModuleResult::Ok);
    ASSERT_EQ(registry.shutdown(), ModuleResult::Ok);
    for (const auto result : context.results) {
        EXPECT_EQ(result, ModuleResult::Busy);
    }
}

TEST(ModuleLifecycleTest, LifecycleTraceIsIdenticalForEveryRegistrationPermutation) {
    std::array<std::size_t, 3> permutation{0, 1, 2};
    std::uint64_t expected_hash = 0;
    do {
        HookLog log;
        std::array<HookContext, 3> contexts{{{&log, 1, true}, {&log, 2, true}, {&log, 3, true}}};
        const std::array<ModuleDescriptor, 3> modules{Module("z", {"service"}, {}, &contexts[0]),
                                                      Module("a", {}, {"service"}, &contexts[1]),
                                                      Module("b", {}, {}, &contexts[2])};
        ModuleRegistry registry;
        for (const auto index : permutation) {
            ASSERT_EQ(registry.register_module(modules[index]), ModuleResult::Ok);
        }
        ASSERT_EQ(registry.startup(), ModuleResult::Ok);
        ASSERT_EQ(registry.shutdown(), ModuleResult::Ok);
        EXPECT_EQ(log.values(), (std::vector<int>{3, 1, 2, -2, -1, -3}));
        if (expected_hash == 0) {
            expected_hash = registry.execution_trace_hash();
        }
        EXPECT_EQ(registry.execution_trace_hash(), expected_hash);
    } while (std::next_permutation(permutation.begin(), permutation.end()));
}

TEST(ModuleLifecycleTest, NamedPhasesUseDependencyOrderAndIdTiesAcrossEveryPermutation) {
    const std::array<PhaseTask, 4> tasks{{{"z", "PrePhysics", {}},
                                          {"a", "PrePhysics", {"z"}},
                                          {"b", "PrePhysics", {}},
                                          {"tick", "PostTick", {"a"}}}};
    std::array<std::size_t, 4> permutation{0, 1, 2, 3};
    std::uint64_t expected_hash = 0;
    do {
        PhaseScheduler scheduler;
        std::vector<PhaseTask> shuffled;
        for (const auto index : permutation) {
            shuffled.push_back(tasks[index]);
        }
        ASSERT_EQ(scheduler.compile({"PrePhysics", "Physics", "PostTick"}, shuffled),
                  ScheduleResult::Ok);
        EXPECT_EQ(RunPhase(scheduler, 0), (std::vector<std::string>{"b", "z", "a"}));
        EXPECT_TRUE(RunPhase(scheduler, 1).empty());
        EXPECT_EQ(RunPhase(scheduler, 2), (std::vector<std::string>{"tick"}));
        if (expected_hash == 0) {
            expected_hash = scheduler.execution_trace_hash();
        }
        EXPECT_EQ(scheduler.execution_trace_hash(), expected_hash);
    } while (std::next_permutation(permutation.begin(), permutation.end()));
}

TEST(ModuleLifecycleTest, NewlyReadyLowerIdPrecedesAnAlreadyReadyHigherId) {
    PhaseScheduler scheduler;
    ASSERT_EQ(scheduler.compile({"PrePhysics", "PostTick"},
                                {{"z", "PrePhysics", {}},
                                 {"a", "PrePhysics", {"z", "z"}},
                                 {"b", "PrePhysics", {"z"}},
                                 {"tick", "PostTick", {"b"}}}),
              ScheduleResult::Ok);
    EXPECT_EQ(RunPhase(scheduler, 0), (std::vector<std::string>{"z", "a", "b"}));
    EXPECT_EQ(RunPhase(scheduler, 1), (std::vector<std::string>{"tick"}));
    EXPECT_EQ(scheduler.execution_trace_hash(), 0x2405b2651614b029ULL);
}

TEST(ModuleLifecycleTest, InvalidGraphsPreserveThePreviouslyCompiledScheduleAndTrace) {
    struct InvalidGraph {
        std::vector<std::string> phases;
        std::vector<PhaseTask> tasks;
        ScheduleResult result;
    };
    const std::vector<InvalidGraph> cases{
        {{"P", "P"}, {}, ScheduleResult::DuplicateId},
        {{"P"}, {{"a", "P", {}}, {"a", "P", {}}}, ScheduleResult::DuplicateId},
        {{"P"}, {{"a", "Q", {}}}, ScheduleResult::UnknownPhase},
        {{"P"}, {{"a", "P", {"missing"}}}, ScheduleResult::MissingDependency},
        {{"P", "Q"}, {{"a", "P", {"b"}}, {"b", "Q", {}}}, ScheduleResult::LaterPhaseDependency},
        {{"P"}, {{"a", "P", {"a"}}}, ScheduleResult::Cycle},
        {{"P"}, {{"a", "P", {"b"}}, {"b", "P", {"a"}}}, ScheduleResult::Cycle},
        {{""}, {}, ScheduleResult::InvalidIdentifier},
        {{"P"}, {{"", "P", {}}}, ScheduleResult::InvalidIdentifier},
        {{"P"}, {{"a", "", {}}}, ScheduleResult::InvalidIdentifier},
        {{"P"}, {{"a", "P", {""}}}, ScheduleResult::InvalidIdentifier},
        {{"P"}, {{"a", "P", {std::string(129, 'a')}}}, ScheduleResult::InvalidIdentifier},
        {{"P"}, {{"has space", "P", {}}}, ScheduleResult::InvalidIdentifier},
        {{std::string(129, 'a')}, {}, ScheduleResult::InvalidIdentifier},
        {{"P"}, {{"a", std::string(129, 'a'), {}}}, ScheduleResult::InvalidIdentifier},
    };
    for (const auto& entry : cases) {
        PhaseScheduler scheduler;
        ASSERT_EQ(scheduler.compile({"P"}, {{"original", "P", {}}}), ScheduleResult::Ok);
        const auto hash = scheduler.execution_trace_hash();
        EXPECT_EQ(scheduler.compile(entry.phases, entry.tasks), entry.result);
        EXPECT_EQ(scheduler.execution_trace_hash(), hash);
        EXPECT_EQ(RunPhase(scheduler, 0), (std::vector<std::string>{"original"}));
    }
}

TEST(ModuleLifecycleTest, ExecuteRejectsUnknownPhaseAndReentryAndCompileDuringExecution) {
    PhaseScheduler scheduler;
    std::size_t visits = 0;
    EXPECT_EQ(scheduler.execute(0, [&](std::string_view) { ++visits; }),
              ScheduleResult::UnknownPhase);
    ASSERT_EQ(scheduler.compile({"P"}, {{"a", "P", {}}}), ScheduleResult::Ok);
    const auto hash = scheduler.execution_trace_hash();
    EXPECT_EQ(scheduler.execute(1, [&](std::string_view) { ++visits; }),
              ScheduleResult::UnknownPhase);
    EXPECT_EQ(scheduler.execution_trace_hash(), hash);
    EXPECT_EQ(scheduler.execute(0,
                                [&](std::string_view) {
                                    ++visits;
                                    EXPECT_EQ(
                                        scheduler.execute(0, [&](std::string_view) { ++visits; }),
                                        ScheduleResult::Busy);
                                    EXPECT_EQ(scheduler.compile({}, {}), ScheduleResult::Busy);
                                }),
              ScheduleResult::Ok);
    EXPECT_EQ(visits, 1U);
    EXPECT_EQ(RunPhase(scheduler, 0), (std::vector<std::string>{"a"}));
}

TEST(ModuleLifecycleTest, ThrowingVisitorReleasesExecutionGuard) {
    PhaseScheduler scheduler;
    ASSERT_EQ(scheduler.compile({"P"}, {{"a", "P", {}}}), ScheduleResult::Ok);
    EXPECT_THROW(scheduler.execute(0, [](std::string_view) { throw std::runtime_error("stop"); }),
                 std::runtime_error);
    EXPECT_EQ(RunPhase(scheduler, 0), (std::vector<std::string>{"a"}));
    EXPECT_EQ(scheduler.compile({}, {}), ScheduleResult::Ok);
}

TEST(ModuleLifecycleTest, EmptyPhasesAreValidAndVisitsAreLimitedToSelectedParticipants) {
    PhaseScheduler scheduler;
    EXPECT_EQ(scheduler.compile({}, {}), ScheduleResult::Ok);
    std::vector<PhaseTask> tasks;
    for (std::size_t index = 0; index < PhaseScheduler::MaxTasks - 1; ++index) {
        tasks.push_back({"other" + std::to_string(index), "Other", {}});
    }
    tasks.push_back({"selected", "Selected", {}});
    ASSERT_EQ(scheduler.compile({"Other", "Empty", "Selected"}, tasks), ScheduleResult::Ok);
    const auto hash = scheduler.execution_trace_hash();
    EXPECT_TRUE(RunPhase(scheduler, 1).empty());
    EXPECT_EQ(scheduler.execution_trace_hash(), hash);
    EXPECT_EQ(RunPhase(scheduler, 2), (std::vector<std::string>{"selected"}));
    EXPECT_EQ(RunPhase(scheduler, 0).size(), PhaseScheduler::MaxTasks - 1);
}

TEST(ModuleLifecycleTest, TraceSeparatesEventPhaseIdAndExecutionOrder) {
    Luminumbra::ExecutionTrace first;
    Luminumbra::ExecutionTrace second;
    first.record("ab", "c", "d");
    second.record("a", "bc", "d");
    EXPECT_NE(first.hash(), second.hash());
    Luminumbra::ExecutionTrace third;
    third.record("ab", "d", "c");
    EXPECT_NE(first.hash(), third.hash());
    PhaseScheduler scheduler;
    ASSERT_EQ(scheduler.compile({"P"}, {{"a", "P", {}}, {"b", "P", {}}}), ScheduleResult::Ok);
    RunPhase(scheduler, 0);
    const auto original_hash = scheduler.execution_trace_hash();
    PhaseScheduler reordered;
    ASSERT_EQ(reordered.compile({"P"}, {{"a", "P", {"b"}}, {"b", "P", {}}}), ScheduleResult::Ok);
    RunPhase(reordered, 0);
    EXPECT_NE(reordered.execution_trace_hash(), original_hash);
    PhaseScheduler renamed_phase;
    ASSERT_EQ(renamed_phase.compile({"Q"}, {{"a", "Q", {}}, {"b", "Q", {}}}), ScheduleResult::Ok);
    RunPhase(renamed_phase, 0);
    EXPECT_NE(renamed_phase.execution_trace_hash(), original_hash);
    PhaseScheduler renamed_task;
    ASSERT_EQ(renamed_task.compile({"P"}, {{"a", "P", {}}, {"c", "P", {}}}), ScheduleResult::Ok);
    RunPhase(renamed_task, 0);
    EXPECT_NE(renamed_task.execution_trace_hash(), original_hash);
}

TEST(ModuleLifecycleTest, IdentifiersAreBoundedAsciiAndInvalidDeclarationsDoNotRegister) {
    const std::vector<std::string> invalid{"",
                                           "has space",
                                           "slash/name",
                                           std::string(129, 'a'),
                                           std::string("nul\0tail", 8),
                                           std::string(1, '\xff')};
    for (const auto& id : invalid) {
        EXPECT_FALSE(Luminumbra::IsScheduleIdentifier(id));
        ModuleRegistry registry;
        EXPECT_EQ(registry.register_module(Module(id)), ModuleResult::InvalidIdentifier);
        EXPECT_EQ(registry.register_module(Module("a", {id})), ModuleResult::InvalidIdentifier);
        EXPECT_EQ(registry.register_module(Module("a", {}, {id})), ModuleResult::InvalidIdentifier);
        EXPECT_EQ(registry.size(), 0U);
    }
    EXPECT_TRUE(Luminumbra::IsScheduleIdentifier("Az09_.-:module"));
    EXPECT_TRUE(Luminumbra::IsScheduleIdentifier(std::string(128, 'a')));
}

TEST(ModuleLifecycleTest, SchedulerBoundsAcceptExactLimitsAndRejectExcessTransactionally) {
    PhaseScheduler scheduler;
    std::vector<std::string> phases;
    for (std::size_t index = 0; index < PhaseScheduler::MaxPhases; ++index) {
        phases.push_back("phase" + std::to_string(index));
    }
    std::vector<PhaseTask> tasks;
    for (std::size_t index = 0; index < PhaseScheduler::MaxTasks; ++index) {
        tasks.push_back({"task" + std::to_string(index), phases.front(), {}});
    }
    tasks.back().after.assign(PhaseScheduler::MaxDependencies, tasks.front().id);
    ASSERT_EQ(scheduler.compile(phases, tasks), ScheduleResult::Ok);
    const auto original = scheduler.ordered_ids();
    phases.push_back("excess");
    EXPECT_EQ(scheduler.compile(phases, tasks), ScheduleResult::LimitExceeded);
    phases.pop_back();
    tasks.back().after.push_back(tasks.front().id);
    EXPECT_EQ(scheduler.compile(phases, tasks), ScheduleResult::LimitExceeded);
    tasks.back().after.pop_back();
    tasks.push_back({"excess", phases.front(), {}});
    EXPECT_EQ(scheduler.compile(phases, tasks), ScheduleResult::LimitExceeded);
    EXPECT_EQ(scheduler.ordered_ids(), original);
}

TEST(ModuleLifecycleTest, RegistryBoundsAcceptExactLimitsAndRejectExcessWithoutMutation) {
    ModuleRegistry registry;
    ModuleDescriptor descriptor{"provider", {}, {}, nullptr, nullptr, nullptr};
    for (std::size_t index = 0; index < ModuleRegistry::MaxServices; ++index) {
        descriptor.provides.push_back("service" + std::to_string(index));
    }
    ASSERT_EQ(registry.register_module(descriptor), ModuleResult::Ok);
    auto excess = descriptor;
    excess.id = "excess";
    excess.provides.push_back("excess");
    EXPECT_EQ(registry.register_module(excess), ModuleResult::LimitExceeded);
    descriptor.id = "consumer";
    descriptor.requires_services = descriptor.provides;
    descriptor.provides.clear();
    excess = descriptor;
    excess.requires_services.push_back("excess");
    EXPECT_EQ(registry.register_module(excess), ModuleResult::LimitExceeded);
    ASSERT_EQ(registry.register_module(descriptor), ModuleResult::Ok);
    for (std::size_t index = registry.size(); index < ModuleRegistry::MaxModules; ++index) {
        ASSERT_EQ(registry.register_module(
                      {"module" + std::to_string(index), {}, {}, nullptr, nullptr, nullptr}),
                  ModuleResult::Ok);
    }
    EXPECT_EQ(registry.register_module({"excess", {}, {}, nullptr, nullptr, nullptr}),
              ModuleResult::LimitExceeded);
    EXPECT_EQ(registry.size(), ModuleRegistry::MaxModules);
    EXPECT_EQ(registry.startup(), ModuleResult::Ok);
    EXPECT_EQ(registry.shutdown(), ModuleResult::Ok);
}

} // namespace
