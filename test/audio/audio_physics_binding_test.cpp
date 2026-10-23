// Regression for the per-frame audio spatial-clustering log defect.
//
// FrameAudio::UpdateListenerEnvironment calls MiniaudioManager::SetPhysicsSystem on
// every in-game frame so that a world (re)load rebinds the occlusion raycaster without
// extra plumbing. The binding must be edge-triggered to avoid synchronous logger
// writes per rendered frame on the render thread for unchanged bindings. The log
// record is emitted only when the binding changes, which the manager exposes as a
// change counter so the test does not have to intercept the shared logger.
//
// Device-free: initialization uses either a refusing initializer or a silent
// miniaudio engine configured with noDevice, so these tests never open an audio device.
#include <glm/vec3.hpp>
#include <gtest/gtest.h>

#include "audio/MiniaudioManager.h"
#include "core/Log.h"
#include "luminumbra_common/components/audio/AudioTypes.h"

#include <spdlog/sinks/base_sink.h>

#include "../support/UniqueTempPath.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
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

TEST(AudioPhysicsBinding, FailedDeviceInitializationReportsUnavailableAndSuppressesPlayback) {
    auto previous = Log::GetCoreLogger();
    Log::GetCoreLogger() =
        std::make_shared<spdlog::logger>("AUDIO_FAILURE_PROBE", std::make_shared<CountingSink>());
    Luminumbra::Client::MiniaudioManager manager(".");
    const bool initialized =
        manager.Init([](const ma_engine_config*, ma_engine*) -> ma_result { return MA_NO_DEVICE; });
    Log::GetCoreLogger() = previous;
    EXPECT_FALSE(initialized);
    EXPECT_FALSE(manager.IsPlaybackEnabled());
    manager.SetMasterVolume(1.0f);
    manager.SetMusicVolume(1.0f);
    manager.SetSfxVolume(1.0f);
    EXPECT_FALSE(manager.PlayOneShot2D("ui_click"));
    manager.Shutdown();
    EXPECT_FALSE(manager.IsPlaybackEnabled());
}

namespace {
std::atomic<std::size_t>& RejectedAudioAllocationSize() {
    static std::atomic<std::size_t> rejected_size{0};
    return rejected_size;
}

std::atomic<std::size_t>& AudioAllocations() {
    static std::atomic<std::size_t> allocations{0};
    return allocations;
}

void* CountAudioMalloc(std::size_t size, void*) {
    if (size == 0 || size == RejectedAudioAllocationSize().load())
        return nullptr;
    void* allocation = std::malloc(size);
    if (allocation)
        ++AudioAllocations();
    return allocation;
}

void CountAudioFree(void* allocation, void*) {
    if (allocation)
        --AudioAllocations();
    std::free(allocation);
}

void* CountAudioRealloc(void* allocation, std::size_t size, void*) {
    if (size == 0) {
        CountAudioFree(allocation, nullptr);
        return nullptr;
    }
    if (size == RejectedAudioAllocationSize().load())
        return nullptr;
    const bool was_empty = allocation == nullptr;
    void* replacement = std::realloc(allocation, size);
    if (replacement && was_empty)
        ++AudioAllocations();
    return replacement;
}

ma_result InitSilentAudio(const ma_engine_config*, ma_engine* engine) {
    auto config = ma_engine_config_init();
    config.noDevice = MA_TRUE;
    config.channels = 2;
    config.sampleRate = 48000;
    config.allocationCallbacks = {nullptr, CountAudioMalloc, CountAudioRealloc, CountAudioFree};
    return ma_engine_init(&config, engine);
}
} // namespace

TEST(AudioPhysicsBinding, FailedFileLoadsReleaseAllocationsAndLeaveNoWindNode) {
    EXPECT_EQ(CountAudioMalloc(0, nullptr), nullptr);
    const Luminumbra::TestSupport::ScopedTempFile temp("audio-load-", ".json");
    const auto root = temp.Path().parent_path();
    {
        std::ofstream output(temp.Path());
        output << R"({"events":{"missing":{"files":["missing.ogg"]},
                   "invalid":{"files":["invalid.ogg"]},
                   "empty":{"files":["empty.ogg"]},
                   "valid":{"files":["valid.wav"]}}})";
        ASSERT_TRUE(output.good());
    }
    {
        const std::ofstream output(root / "empty.ogg", std::ios::binary);
        ASSERT_TRUE(output.good());
    }
    {
        // Two mono PCM16 samples at 48 kHz; all RIFF lengths match the fixed 48-byte file.
        constexpr std::array<unsigned char, 48> wave = {
            'R', 'I', 'F', 'F', 40,  0,   0,   0,   'W',  'A',  'V', 'E', 'f', 'm',  't', ' ',
            16,  0,   0,   0,   1,   0,   1,   0,   0x80, 0xbb, 0,   0,   0,   0x77, 1,   0,
            2,   0,   16,  0,   'd', 'a', 't', 'a', 4,    0,    0,   0,   0,   0,    0,   0};
        std::ofstream output(root / "valid.wav", std::ios::binary);
        output.write(reinterpret_cast<const char*>(wave.data()),
                     static_cast<std::streamsize>(wave.size()));
        ASSERT_TRUE(output.good());
    }
    {
        std::ofstream output(root / "invalid.ogg", std::ios::binary);
        output << "partial audio bytes";
        ASSERT_TRUE(output.good());
    }
    auto previous = Log::GetCoreLogger();
    Log::GetCoreLogger() =
        std::make_shared<spdlog::logger>("audio-load", std::make_shared<CountingSink>());
    {
        Luminumbra::Client::MiniaudioManager manager(root.generic_string() + "/");
        const bool initialized = manager.Init(InitSilentAudio);
        EXPECT_TRUE(initialized);
        if (initialized) {
            EXPECT_TRUE(manager.LoadBank(temp.Path().filename().generic_string()));
            // Engine init may return before the resource worker frees its thread-start
            // proxy. A successful stream and ordered stop synchronize that startup
            // before capturing the steady-state baseline on Windows and POSIX.
            const auto startup_allocations = AudioAllocations().load();
            manager.PlayMusic("valid");
            EXPECT_GT(AudioAllocations().load(), startup_allocations);
            manager.StopMusic();
            const auto baseline = AudioAllocations().load();
            // Reuse the same paths after refusal, including the streamed music path.
            // Every failed load must release its own resources before returning.
            for (const char* event : {"missing", "invalid", "empty", "missing", "invalid"}) {
                SCOPED_TRACE(event);
                Luminumbra::Common::AudioEventHandle handle{};
                EXPECT_FALSE(manager.PlayEvent(event, handle));
                EXPECT_EQ(AudioAllocations().load(), baseline);
                EXPECT_FALSE(manager.PlayOneShot2D(event));
                EXPECT_EQ(AudioAllocations().load(), baseline);
                EXPECT_FALSE(manager.PlayOneShot(event, glm::vec3(0)));
                EXPECT_EQ(AudioAllocations().load(), baseline);
                manager.PlayMusic(event);
                EXPECT_EQ(AudioAllocations().load(), baseline);
                manager.PlayAmbientLoop(event, glm::vec3(0), 10.0f);
                EXPECT_EQ(AudioAllocations().load(), baseline);
            }
            manager.SetWindParameters(glm::vec3(1), 1.0f);
            EXPECT_EQ(AudioAllocations().load(), baseline);
            manager.SetWindParameters(glm::vec3(1), 0.0f);
            // Fail the path copy before any load job is posted. Cleanup must not
            // wait for a job which never ran, and the next load must still work.
            RejectedAudioAllocationSize().store((root / "valid.wav").generic_string().size() + 1);
            manager.PlayMusic("valid");
            RejectedAudioAllocationSize().store(0);
            EXPECT_EQ(AudioAllocations().load(), baseline);
            // 0.11.22 allocates two one-second float32 stream pages. Exercise failure
            // after decoder init, then successful reuse and explicit node teardown.
            RejectedAudioAllocationSize().store(std::size_t{2} * 48000 * sizeof(float));
            manager.PlayMusic("valid");
            RejectedAudioAllocationSize().store(0);
            EXPECT_EQ(AudioAllocations().load(), baseline);
            manager.PlayMusic("valid");
            EXPECT_GT(AudioAllocations().load(), baseline);
            manager.StopMusic();
            EXPECT_EQ(AudioAllocations().load(), baseline);
        }
        manager.Shutdown();
    }
    Log::GetCoreLogger() = previous;
    EXPECT_EQ(AudioAllocations().load(), 0u);
}
