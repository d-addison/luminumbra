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

#include <cstdint>

namespace {

// Distinct non-null pointer values; the pointee is never dereferenced by the binding.
::Luminumbra::Systems::PhysicsSystem* FakePhysics(std::uintptr_t token) {
    return reinterpret_cast<::Luminumbra::Systems::PhysicsSystem*>(token);
}

} // namespace

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
