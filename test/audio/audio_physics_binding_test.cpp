// Regression for the per-frame audio spatial-clustering log defect.
//
// FrameAudio::UpdateListenerEnvironment calls MiniaudioManager::SetPhysicsSystem on
// every in-game frame so that a world (re)load rebinds the occlusion raycaster without
// extra plumbing. Before the fix the call logged an INFO line unconditionally, i.e. one
// synchronous logger write per rendered frame on the render thread (the PID 66604
// session logged that line 77,659 times). The binding must be edge-triggered: the log
// record is emitted only when the binding changes, which the manager exposes as a
// change counter so the test does not have to intercept the shared logger.
//
// Device-free: MiniaudioManager's constructor only creates the spatial cluster; Init()
// (which opens a miniaudio engine/device) is never called here.
#include <gtest/gtest.h>

#include "audio/MiniaudioManager.h"
#include "core/Log.h"

#include <spdlog/sinks/base_sink.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace {

// Counts binding log records without touching the shared logger's sink list: a
// private logger with the same sink type is installed for the duration of the test
// via the manager's own logging path, so nothing races the periodic flusher.
class CountingSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::size_t count = 0;

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        const std::string text(msg.payload.data(), msg.payload.size());
        if (text.find("audio spatial clustering") != std::string::npos)
            ++count;
    }
    void flush_() override {}
};

// Distinct non-null pointer values; the pointee is never dereferenced by the binding.
::Luminumbra::Systems::PhysicsSystem* FakePhysics(std::uintptr_t token) {
    return reinterpret_cast<::Luminumbra::Systems::PhysicsSystem*>(token);
}

} // namespace

TEST(AudioPhysicsBinding, RepeatedBindingLogsOnce) {
    // Swap in a private logger for the duration of the test, restoring the previous
    // one afterwards, so no sink is added to or removed from a logger that a
    // background flusher may be using.
    auto sink = std::make_shared<CountingSink>();
    auto probe = std::make_shared<spdlog::logger>("LUMINUMBRA_BINDING_PROBE", sink);
    probe->set_level(spdlog::level::trace);
    // GetCoreLogger() returns a reference to the static handle, so the swap is a
    // plain assignment; the probe logger is never registered with spdlog, so the
    // periodic flusher never touches it.
    auto previous = Log::GetCoreLogger();
    Log::GetCoreLogger() = probe;
    {
        Luminumbra::Client::MiniaudioManager manager(".");
        auto* physics = FakePhysics(0x1000);
        for (int frame = 0; frame < 1000; ++frame) {
            manager.SetPhysicsSystem(physics);
        }
        manager.SetPhysicsSystem(FakePhysics(0x2000));
    }
    Log::GetCoreLogger() = previous;
    EXPECT_EQ(sink->count, 2u); // one bind, one rebind; never one per frame
}

TEST(AudioPhysicsBinding, RepeatedBindingCountsOnce) {
    Luminumbra::Client::MiniaudioManager manager(".");
    auto* physics = FakePhysics(0x1000);
    for (int frame = 0; frame < 1000; ++frame) {
        manager.SetPhysicsSystem(physics);
    }
    EXPECT_EQ(manager.physics_binding_changes(), 1u);
}

TEST(AudioPhysicsBinding, CountsOnlyPointerChanges) {
    Luminumbra::Client::MiniaudioManager manager(".");
    manager.SetPhysicsSystem(nullptr); // no change from the initial unbound state
    EXPECT_EQ(manager.physics_binding_changes(), 0u);
    manager.SetPhysicsSystem(FakePhysics(0x1000));
    manager.SetPhysicsSystem(FakePhysics(0x1000));
    manager.SetPhysicsSystem(FakePhysics(0x2000)); // world reload rebinds to a new system
    manager.SetPhysicsSystem(nullptr);             // world unloaded
    manager.SetPhysicsSystem(nullptr);
    EXPECT_EQ(manager.physics_binding_changes(), 3u);
}
