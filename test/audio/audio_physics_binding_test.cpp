// Regression for the per-frame audio spatial-clustering log defect.
//
// FrameAudio::UpdateListenerEnvironment calls MiniaudioManager::SetPhysicsSystem on
// every in-game frame so that a world (re)load rebinds the occlusion raycaster without
// extra plumbing. Before the fix the call logged an INFO line unconditionally, i.e. one
// synchronous logger write per rendered frame on the render thread (the PID 66604
// session logged that line 77,659 times). The binding must be edge-triggered: one log
// record per pointer change, none for repeats.
//
// Device-free: MiniaudioManager's constructor only creates the spatial cluster; Init()
// (which opens a miniaudio engine/device) is never called here.
#include <gtest/gtest.h>

#include "audio/MiniaudioManager.h"
#include "core/Log.h"

#include <spdlog/sinks/ostream_sink.h>

#include <memory>
#include <sstream>
#include <string>

namespace {

class LogCapture {
public:
    LogCapture()
        : m_sink(std::make_shared<spdlog::sinks::ostream_sink_mt>(m_stream)) {
        m_sink->set_pattern("%v");
        Log::GetCoreLogger()->sinks().push_back(m_sink);
    }
    ~LogCapture() {
        auto& sinks = Log::GetCoreLogger()->sinks();
        for (auto it = sinks.begin(); it != sinks.end(); ++it) {
            if (*it == m_sink) {
                sinks.erase(it);
                break;
            }
        }
    }
    std::size_t count(const std::string& needle) {
        Log::GetCoreLogger()->flush();
        const std::string text = m_stream.str();
        std::size_t hits = 0;
        for (std::size_t pos = text.find(needle); pos != std::string::npos;
             pos = text.find(needle, pos + needle.size())) {
            ++hits;
        }
        return hits;
    }

private:
    std::ostringstream m_stream;
    std::shared_ptr<spdlog::sinks::ostream_sink_mt> m_sink;
};

constexpr const char* kAttached =
    "Physics system integration established for audio spatial clustering";
constexpr const char* kDetached = "Physics system detached from audio spatial clustering";

// Distinct non-null pointer values; the pointee is never dereferenced by the binding.
::Luminumbra::Systems::PhysicsSystem* FakePhysics(std::uintptr_t token) {
    return reinterpret_cast<::Luminumbra::Systems::PhysicsSystem*>(token);
}

} // namespace

TEST(AudioPhysicsBinding, RepeatedBindingLogsOnce) {
    LogCapture capture;
    Luminumbra::Client::MiniaudioManager manager(".");
    auto* physics = FakePhysics(0x1000);
    for (int frame = 0; frame < 1000; ++frame) {
        manager.SetPhysicsSystem(physics);
    }
    EXPECT_EQ(capture.count(kAttached), 1u);
    EXPECT_EQ(capture.count(kDetached), 0u);
}

TEST(AudioPhysicsBinding, LogsOnlyOnPointerChange) {
    LogCapture capture;
    Luminumbra::Client::MiniaudioManager manager(".");
    manager.SetPhysicsSystem(nullptr); // no change from the initial unbound state
    EXPECT_EQ(capture.count(kAttached), 0u);
    EXPECT_EQ(capture.count(kDetached), 0u);

    manager.SetPhysicsSystem(FakePhysics(0x1000));
    manager.SetPhysicsSystem(FakePhysics(0x1000));
    manager.SetPhysicsSystem(FakePhysics(0x2000)); // world reload rebinds to a new system
    manager.SetPhysicsSystem(nullptr);             // world unloaded
    manager.SetPhysicsSystem(nullptr);
    EXPECT_EQ(capture.count(kAttached), 2u);
    EXPECT_EQ(capture.count(kDetached), 1u);
}
