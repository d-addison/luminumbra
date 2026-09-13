#include "../../src/luminumbra_client/rendering/RenderGraph.h"
#include <algorithm>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <gtest/gtest.h>
#include <limits>
#include <luminumbra/rendering/RenderView.h>

namespace {
using namespace Luminumbra::Rendering;
RenderViewDescription Description() {
    RenderViewDescription result;
    result.width = 800;
    result.height = 600;
    result.revision = 7;
    result.near_plane = .1;
    result.far_plane = 1000;
    const auto projection = glm::perspectiveRH_ZO(glm::radians(45.0), 4.0 / 3.0, 1000.0, .1);
    std::copy_n(glm::value_ptr(projection), 16, result.projection.begin());
    return result;
}
bool Inside(const RenderView& view, const glm::vec3& point) {
    for (const auto& plane : view.frustum_planes())
        if (glm::dot(glm::make_vec4(plane.data()), glm::vec4(point, 1)) < 0)
            return false;
    return true;
}
TEST(RenderView, ExactUniformMatricesDriveReversedDepthAndCulling) {
    const auto description = Description();
    const auto view = RenderView::Validate(description);
    EXPECT_EQ(view.description().projection, description.projection);
    const auto projection = glm::make_mat4(view.projection().data());
    const auto near = projection * glm::vec4(0, 0, -.1f, 1);
    const auto far = projection * glm::vec4(0, 0, -1000.f, 1);
    EXPECT_NEAR(near.z / near.w, 1.0f, 1e-6f);
    EXPECT_NEAR(far.z / far.w, 0.0f, 1e-7f);
    EXPECT_TRUE(Inside(view, {0, 0, -1}));
    EXPECT_FALSE(Inside(view, {0, 0, -.01f}));
    EXPECT_FALSE(Inside(view, {0, 0, -1001.f}));
    EXPECT_FALSE(Inside(view, {10, 0, -1}));
    EXPECT_FALSE(Inside(view, {0, 0, 1}));
}
TEST(RenderView, EyeAndRolledViewRemainJoinedWithoutEulerReconstruction) {
    auto description = Description();
    const auto matrix = glm::lookAt(
        glm::dvec3(12, 4, 7), glm::dvec3(2, 3, 1), glm::normalize(glm::dvec3(.4, 1, .2)));
    std::copy_n(glm::value_ptr(matrix), 16, description.view.begin());
    const auto view = RenderView::Validate(description);
    EXPECT_NEAR(view.eye()[0], 12.f, 1e-5f);
    EXPECT_NEAR(view.eye()[1], 4.f, 1e-5f);
    EXPECT_NEAR(view.eye()[2], 7.f, 1e-5f);
    EXPECT_TRUE(Inside(view, {2, 3, 1}));
    EXPECT_EQ(view.description().view, description.view);
}
TEST(RenderView, RefusesUnknownOrInconsistentProjectionConventions) {
    for (int mode = 0; mode < 5; ++mode) {
        auto description = Description();
        if (mode == 0)
            description.projection[10] = -1;
        if (mode == 1)
            description.projection[15] = 1;
        if (mode == 2)
            description.projection[8] = .1;
        if (mode == 3)
            description.far_plane = 100;
        if (mode == 4)
            description.projection[10] = 0;
        EXPECT_THROW(RenderView::Validate(description), std::invalid_argument) << mode;
    }
}
TEST(RenderView, RefusesScaleReflectionAndProjectiveViewTransforms) {
    for (const auto value : {2.0, -1.0}) {
        auto description = Description();
        description.view[0] = value;
        EXPECT_THROW(RenderView::Validate(description), std::invalid_argument);
    }
    for (const auto coefficient : {.1, 1e-7, -1e-12}) {
        auto description = Description();
        description.view[3] = coefficient;
        EXPECT_THROW(RenderView::Validate(description), std::invalid_argument);
    }
}
TEST(RenderView, RefusesMissingRevisionBadExtentsAndMismatchedAspect) {
    for (int mode = 0; mode < 5; ++mode) {
        auto description = Description();
        if (mode == 0)
            description.revision = 0;
        if (mode == 1)
            description.width = 0;
        if (mode == 2)
            description.height = 4097;
        if (mode == 3)
            description.width = 600;
        if (mode == 4)
            description.height = 0;
        EXPECT_THROW(RenderView::Validate(description), std::invalid_argument) << mode;
    }
}
TEST(RenderView, RefusesNonfiniteAndUnboundedInputs) {
    for (int mode = 0; mode < 6; ++mode) {
        auto description = Description();
        if (mode == 0)
            description.view[0] = std::numeric_limits<double>::quiet_NaN();
        if (mode == 1)
            description.projection[0] = std::numeric_limits<double>::infinity();
        if (mode == 2)
            description.far_plane = std::numeric_limits<double>::infinity();
        if (mode == 3)
            description.near_plane = 0;
        if (mode == 4)
            description.far_plane = description.near_plane;
        if (mode == 5)
            description.view[12] = 1e20;
        EXPECT_THROW(RenderView::Validate(description), std::invalid_argument) << mode;
    }
}
TEST(StaticDrawGraph, OptionalAuthoredAttachmentConnectsItsActualWritersAndLighting) {
    const auto ordinary = BuildLuminumbraFrameGraph();
    const auto authored = BuildLuminumbraFrameGraph(true);
    EXPECT_TRUE(authored.validate().empty());
    EXPECT_EQ(authored.schedule(), ordinary.schedule());
    for (size_t i = 0; i < authored.nodes().size(); ++i) {
        const auto& node = authored.nodes()[i];
        const auto has = [](const auto& names) {
            return std::find(names.begin(), names.end(), "gbuffer.authored_surface") != names.end();
        };
        EXPECT_FALSE(has(ordinary.nodes()[i].reads));
        EXPECT_FALSE(has(ordinary.nodes()[i].writes));
        EXPECT_EQ(has(node.writes), node.name == "gbuffer" || node.name == "plant_procgen");
        EXPECT_EQ(has(node.reads), node.name == "lighting");
    }
}
} // namespace
