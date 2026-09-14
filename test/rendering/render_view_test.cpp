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
    EXPECT_FALSE(view.orthographic());
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
TEST(RenderView, OffCenterOrthographicPreservesExactMatricesAndParallelDepthCoverage) {
    auto description = Description();
    const auto supplied = glm::orthoRH_ZO(-2.0, 6.0, -2.0, 4.0, 1000.0, .1);
    std::copy_n(glm::value_ptr(supplied), 16, description.projection.begin());
    const auto view = RenderView::Validate(description);
    EXPECT_TRUE(view.orthographic());
    EXPECT_EQ(view.description().projection, description.projection);
    for (size_t index = 0; index < 16; ++index)
        EXPECT_EQ(view.projection()[index], static_cast<float>(description.projection[index]));
    const auto projection = glm::make_mat4(view.projection().data());
    const auto near = projection * glm::vec4(2, 1, -.1f, 1);
    const auto far = projection * glm::vec4(2, 1, -1000.f, 1);
    EXPECT_FLOAT_EQ(near.w, 1);
    EXPECT_FLOAT_EQ(far.w, 1);
    EXPECT_NEAR(near.z, 1, 1e-6);
    EXPECT_NEAR(far.z, 0, 1e-7);
    EXPECT_FLOAT_EQ(near.x, far.x);
    EXPECT_FLOAT_EQ(near.y, far.y);
    EXPECT_FLOAT_EQ(near.x, 0);
    EXPECT_FLOAT_EQ(near.y, 0);
    for (float depth : {-1.f, -500.f}) {
        EXPECT_TRUE(Inside(view, {5.9f, 3.9f, depth}));
        for (const auto point :
             {glm::vec3(-2.1f, 1, depth), {6.1f, 1, depth}, {2, -2.1f, depth}, {2, 4.1f, depth}})
            EXPECT_FALSE(Inside(view, point));
    }
    EXPECT_FALSE(Inside(view, {2, 1, -.01f}));
    EXPECT_FALSE(Inside(view, {2, 1, -1001.f}));
    EXPECT_FALSE(Inside(view, {2, 1, 1}));
}
TEST(RenderView, RolledOrthographicCullingAndViewerDirectionUseTheSuppliedView) {
    auto description = Description();
    const auto supplied = glm::orthoRH_ZO(-4.0, 4.0, -3.0, 3.0, 1000.0, .1);
    const auto camera = glm::lookAt(
        glm::dvec3(12, 4, 7), glm::dvec3(2, 3, 1), glm::normalize(glm::dvec3(.4, 1, .2)));
    std::copy_n(glm::value_ptr(supplied), 16, description.projection.begin());
    std::copy_n(glm::value_ptr(camera), 16, description.view.begin());
    const auto view = RenderView::Validate(description);
    const auto world = glm::make_mat4(view.inverse_view().data());
    const auto viewer_direction = glm::normalize(glm::vec3(world[2]));
    EXPECT_NEAR(glm::dot(viewer_direction, glm::normalize(glm::vec3(10, 1, 6))), 1, 1e-6);
    for (float depth : {-1.f, -500.f}) {
        EXPECT_TRUE(Inside(view, glm::vec3(world * glm::vec4(3.9f, 2.9f, depth, 1))));
        EXPECT_FALSE(Inside(view, glm::vec3(world * glm::vec4(4.1f, 0, depth, 1))));
    }
}
TEST(RenderView, OrthographicMaximumSpanAcceptsFloat32ProjectionAtTheInclusiveBound) {
    auto description = Description();
    const auto supplied = glm::orthoRH_ZO(-1e6f, 1e6f, -750000.f, 750000.f, 1000.f, .1f);
    std::copy_n(glm::value_ptr(supplied), 16, description.projection.begin());
    const auto view = RenderView::Validate(description);
    EXPECT_TRUE(view.orthographic());
    EXPECT_TRUE(Inside(view, {999999.f, 749999.f, -1}));
    EXPECT_FALSE(Inside(view, {1000001.f, 0, -1}));
}
TEST(RenderView, OrthographicRefusesObliqueDepthHybridAspectAndUnboundedSpans) {
    for (int mode = 0; mode < 10; ++mode) {
        auto description = Description();
        const auto supplied = glm::orthoRH_ZO(-4.0, 4.0, -3.0, 3.0, 1000.0, .1);
        std::copy_n(glm::value_ptr(supplied), 16, description.projection.begin());
        if (mode == 0)
            description.projection[10] *= -1;
        if (mode == 1)
            description.projection[11] = -1;
        if (mode == 2)
            description.projection[15] = 0;
        if (mode == 3)
            description.projection[8] = .1;
        if (mode == 4)
            description.projection[1] = .1;
        if (mode == 5)
            description.projection[0] = 0;
        if (mode == 6)
            description.projection[0] = 1e-7;
        if (mode == 7)
            description.projection[0] = 1001;
        if (mode == 8)
            description.projection[12] = 1e7;
        if (mode == 9)
            description.width = 600;
        EXPECT_THROW(RenderView::Validate(description), std::invalid_argument) << mode;
    }
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
