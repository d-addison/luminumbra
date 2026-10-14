#include "luminumbra_client/rendering/RenderPipeline.h"
#include "luminumbra_client/rendering/TimeOfDayModel.h"

#include <gtest/gtest.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <nlohmann/json.hpp>

namespace Luminumbra::Rendering {
struct RenderClockTestPeer {
    static double DayDuration(const RenderPipeline& pipeline) {
        return pipeline.m_dayDurationSeconds;
    }
    static void Update(RenderPipeline& pipeline, float dt) {
        pipeline.update_time_of_day(dt);
    }
};
} // namespace Luminumbra::Rendering

TEST(RenderClock, DefaultPacingAndRestoredCustomCalendarPreserveOverrides) {
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
    EXPECT_DOUBLE_EQ(Rendering::RenderClockTestPeer::DayDuration(pipeline), 5400.0);
    constexpr std::uint64_t default_day_ticks = world::WorldCalendar{}.dayLengthTicks;
    pipeline.set_time_of_day(0.0f);
    // A quarter of the default day agrees for wall-clock and tick-driven paths.
    Rendering::RenderClockTestPeer::Update(pipeline,
                                           static_cast<float>(default_day_ticks / 4) / 30.0f);
    EXPECT_FLOAT_EQ(pipeline.get_time_of_day(), .1875f);
    pipeline.set_time_of_day_tick(default_day_ticks / 4);
    Rendering::RenderClockTestPeer::Update(pipeline, 123.0f);
    EXPECT_FLOAT_EQ(pipeline.get_time_of_day(), .1875f);
    pipeline.set_world_clock(world::WorldClock(default_day_ticks / 2));
    Rendering::RenderClockTestPeer::Update(pipeline, 123.0f);
    EXPECT_FLOAT_EQ(pipeline.get_time_of_day(), 0.0f); // calendar midday == sky noon
    EXPECT_NEAR(pipeline.get_moon_illumination(),
                Rendering::LunarIllumination(default_day_ticks / 2,
                                             world::WorldClock{}.lunar_period_ticks()),
                1e-6f);
    for (const auto tick : {0ull,
                            26999ull,
                            27000ull,
                            27001ull,
                            81000ull,
                            134999ull,
                            135000ull,
                            135001ull,
                            162000ull,
                            648000ull,
                            1295999ull,
                            1296000ull}) {
        const world::WorldClock clock(tick);
        pipeline.set_world_clock(clock);
        Rendering::RenderClockTestPeer::Update(pipeline, 10.0f);
        EXPECT_FLOAT_EQ(pipeline.get_time_of_day(), Rendering::TimeOfDayFromWorldClock(clock));
        EXPECT_FLOAT_EQ(pipeline.get_moon_illumination(),
                        Rendering::LunarIllumination(tick, clock.lunar_period_ticks()));
        const auto noon_tick = (tick + default_day_ticks / 2) % default_day_ticks;
        pipeline.set_time_of_day_tick(noon_tick);
        Rendering::RenderClockTestPeer::Update(pipeline, 10.0f);
        EXPECT_FLOAT_EQ(pipeline.get_time_of_day(), Rendering::TimeOfDayFromWorldClock(clock));
        pipeline.set_time_of_day(0.0f);
        Rendering::RenderClockTestPeer::Update(pipeline, static_cast<float>(noon_tick) / 30.0f);
        EXPECT_NEAR(pipeline.get_time_of_day(), Rendering::TimeOfDayFromWorldClock(clock), 1e-7f);
    }
    pipeline.set_moon_illumination(.42f);
    pipeline.set_world_clock(world::WorldClock(648000));
    Rendering::RenderClockTestPeer::Update(pipeline, 0.0f);
    EXPECT_FLOAT_EQ(pipeline.get_moon_illumination(), .42f);
    pipeline.set_moon_illumination(5.0f);
    Rendering::RenderClockTestPeer::Update(pipeline, 0.0f);
    EXPECT_FLOAT_EQ(pipeline.get_moon_illumination(), 1.0f);
    pipeline.set_moon_illumination(-1.0f);
    // A direct solar-angle pin resumes continuously via its inverse elapsed phase.
    pipeline.set_time_of_day(.3f);
    pipeline.set_time_of_day_hold(true);
    Rendering::RenderClockTestPeer::Update(pipeline, 5400.0f);
    EXPECT_FLOAT_EQ(pipeline.get_time_of_day(), .3f);
    pipeline.set_time_of_day_hold(false);
    Rendering::RenderClockTestPeer::Update(pipeline, 0.0f);
    EXPECT_FLOAT_EQ(pipeline.get_time_of_day(), .3f);
    Rendering::RenderClockTestPeer::Update(pipeline, 90.0f);
    EXPECT_NEAR(pipeline.get_time_of_day(), .325f, 1e-6f); // night rate: 1.5 solar cycles/day
    // Both legacy setters independently clear the world season authority.
    pipeline.set_time_of_day_tick(27000);
    Rendering::RenderClockTestPeer::Update(pipeline, 100.0f);
    EXPECT_FLOAT_EQ(pipeline.get_season_phase(), Rendering::ComputeSeason(648000, 432000).phase);
    pipeline.set_render_calendar(world::WorldCalendar::Legacy());
    pipeline.set_time_of_day(0.0f);
    Rendering::RenderClockTestPeer::Update(pipeline, 1800.0f);
    EXPECT_FLOAT_EQ(pipeline.get_time_of_day(), .25f);
    pipeline.set_time_of_day_tick(54000);
    Rendering::RenderClockTestPeer::Update(pipeline, 100.0f);
    EXPECT_FLOAT_EQ(pipeline.get_time_of_day(), .25f);
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
    EXPECT_FLOAT_EQ(pipeline.get_moon_illumination(),
                    Rendering::LunarIllumination(restored.tick(), restored.lunar_period_ticks()));
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
    const world::WorldClock asymmetric(135000);
    pipeline.set_day_length_ticks(default_day_ticks * 2);
    pipeline.set_world_clock(asymmetric);
    Rendering::RenderClockTestPeer::Update(pipeline, 100.0f);
    EXPECT_FLOAT_EQ(pipeline.get_time_of_day(), .9375f);
    EXPECT_FLOAT_EQ(pipeline.get_season_phase(), Rendering::ComputeSeason(asymmetric).phase);
    EXPECT_FLOAT_EQ(
        pipeline.get_moon_illumination(),
        Rendering::LunarIllumination(asymmetric.tick(), asymmetric.lunar_period_ticks()));
}
