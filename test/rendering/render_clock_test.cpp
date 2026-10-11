#include "luminumbra_client/rendering/RenderPipeline.h"
#include "luminumbra_client/rendering/TimeOfDayModel.h"

#include <gtest/gtest.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>

namespace Luminumbra::Rendering {
struct RenderClockTestPeer {
    static void Update(RenderPipeline& pipeline, float dt) {
        pipeline.update_time_of_day(dt);
    }
};
} // namespace Luminumbra::Rendering

TEST(RenderClock, RestoredCustomCalendarDrivesSkyAndPreservesOverrides) {
    ASSERT_TRUE(glfwInit());
    struct GlLifetime {
        GLFWwindow* window = nullptr;
        ~GlLifetime() {
            if (window)
                glfwDestroyWindow(window);
            glfwTerminate();
        }
    } gl;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    gl.window = glfwCreateWindow(16, 16, "clock", nullptr, nullptr);
    ASSERT_NE(gl.window, nullptr);
    glfwMakeContextCurrent(gl.window);
    ASSERT_TRUE(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)));

    using namespace Luminumbra;
    Rendering::RenderPipeline pipeline;
    world::WorldClock restored;
    std::string error;
    ASSERT_TRUE(world::WorldClock::from_metadata(
        nlohmann::json::parse(
            R"({"simulationTick":317,"calendar":{"dayLengthTicks":120,"daysPerYear":4}})"),
        restored,
        error));
    for (const auto tick : {317ull, 318ull, 330ull, 360ull, 390ull, 480ull}) {
        SCOPED_TRACE(tick);
        restored.set_tick(tick);
        pipeline.set_world_clock(restored);
        Rendering::RenderClockTestPeer::Update(pipeline, 123.0f);
        const float day = static_cast<float>(tick % 120) / 120.0f;
        const float year = static_cast<float>(tick % 480) / 480.0f;
        EXPECT_FLOAT_EQ(pipeline.get_time_of_day(), day >= .5f ? day - .5f : day + .5f);
        EXPECT_FLOAT_EQ(pipeline.get_season_phase(), year >= .25f ? year - .25f : year + .75f);
        EXPECT_EQ(pipeline.get_season_tick(), tick);
    }
    restored.set_tick(390);
    pipeline.set_time_of_day(.2f);
    pipeline.set_time_of_day_hold(true);
    pipeline.set_world_clock(restored);
    Rendering::RenderClockTestPeer::Update(pipeline, 123.0f);
    EXPECT_FLOAT_EQ(pipeline.get_time_of_day(), .2f);
    EXPECT_FLOAT_EQ(pipeline.get_season_phase(), .5625f);
    pipeline.set_time_of_day_hold(false);
    pipeline.set_day_length_ticks(60);
    pipeline.set_world_clock(restored);
    Rendering::RenderClockTestPeer::Update(pipeline, 123.0f);
    EXPECT_FLOAT_EQ(pipeline.get_time_of_day(), 0.0f);
    EXPECT_FLOAT_EQ(pipeline.get_season_phase(), .5625f);
    // Later scenario pins still override the clock in the same frame.
    pipeline.set_world_clock(restored);
    pipeline.set_time_of_day(.3f);
    Rendering::RenderClockTestPeer::Update(pipeline, 123.0f);
    EXPECT_FLOAT_EQ(pipeline.get_time_of_day(), .3f);
    // The legacy setters restore the legacy periods and origin.
    pipeline.set_season_tick(390);
    pipeline.set_time_of_day_tick(390);
    Rendering::RenderClockTestPeer::Update(pipeline, 123.0f);
    EXPECT_FLOAT_EQ(pipeline.get_time_of_day(), .5f);
    EXPECT_FLOAT_EQ(pipeline.get_season_phase(), Rendering::ComputeSeason(390, 432000).phase);
}
