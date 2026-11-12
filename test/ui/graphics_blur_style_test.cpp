#include "ui/Rml_UIManager.h"

#include <gtest/gtest.h>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/ComputedValues.h>
#include <RmlUi/Core/Filter.h>
#include <RmlUi/Core/PropertyDefinition.h>
#include <RmlUi/Core/StyleSheetTypes.h>

#include <array>
#include <memory>
#include <string>

namespace {
using luminumbra::core::UiBlurQuality;

class GraphicsBlurStyle : public testing::Test {
protected:
    GLFWwindow* window = nullptr;
    bool glfw_initialized = false;
    std::unique_ptr<Luminumbra::Client::Rml_UIManager> ui;

    void SetUp() override {
        ASSERT_TRUE(glfwInit());
        glfw_initialized = true;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window = glfwCreateWindow(800, 600, "graphics_blur_style_test", nullptr, nullptr);
        ASSERT_NE(window, nullptr);
        glfwMakeContextCurrent(window);
        ASSERT_TRUE(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)));
        ui = std::make_unique<Luminumbra::Client::Rml_UIManager>(
            std::string(LUMINUMBRA_SOURCE_ROOT) + "/");
        ui->Init(window, nullptr);
        ASSERT_NE(ui->GetContext(), nullptr);
    }

    void TearDown() override {
        if (ui) {
            ui->Shutdown();
            ui.reset();
        }
        if (window) {
            glfwDestroyWindow(window);
        }
        if (glfw_initialized) {
            glfwTerminate();
        }
    }

    static void ExpectBackdrop(Rml::Element* element, float radius) {
        ASSERT_NE(element, nullptr);
        EXPECT_FALSE(element->GetComputedValues().has_filter());
        EXPECT_EQ(element->GetComputedValues().has_backdrop_filter(), radius != 0.0f);
        const auto* property = element->GetProperty(Rml::PropertyId::BackdropFilter);
        ASSERT_NE(property, nullptr);
        ASSERT_EQ(property->unit, Rml::Unit::FILTER);
        const auto filters = property->Get<Rml::FiltersPtr>();
        if (radius == 0.0f) {
            EXPECT_FALSE(filters);
            return;
        }
        ASSERT_TRUE(filters);
        ASSERT_EQ(filters->list.size(), 1u);
        const auto& blur = filters->list.front();
        EXPECT_EQ(blur.type, "blur");
        ASSERT_NE(blur.instancer, nullptr);
        const auto* definition = blur.instancer->GetPropertySpecification().GetProperty("sigma");
        ASSERT_NE(definition, nullptr);
        const auto* sigma = blur.properties.GetProperty(definition->GetId());
        ASSERT_NE(sigma, nullptr);
        EXPECT_FLOAT_EQ(element->ResolveLength(sigma->GetNumericValue()), radius);
    }
};

TEST_F(GraphicsBlurStyle, ComputedBackdropRadiiFollowLoadedAndLiveSelectionsWithoutCompounding) {
    struct ElementSpec {
        const char* selector;
        float full_radius;
    };
    struct PageSpec {
        const char* path;
        const char* id;
        std::array<ElementSpec, 3> elements;
    };
    const std::array pages{
        PageSpec{
            .path = "settings.rml",
            .id = "settings",
            .elements = {{{".settings-panel", 22.0f}, {"#graphics_reset", 12.0f}, {"h1", 0.0f}}}},
        PageSpec{.path = "world_creation.rml",
                 .id = "world_creation",
                 .elements = {{{".creation-panel", 22.0f}, {"#create_btn", 0.0f}, {"h1", 0.0f}}}},
        PageSpec{
            .path = "pause.rml",
            .id = "pause",
            .elements = {
                {{".pause-overlay", 10.0f}, {".pause-panel", 10.0f}, {"#resume_btn", 12.0f}}}}};
    for (const auto initial : {UiBlurQuality::Off, UiBlurQuality::Half, UiBlurQuality::Full}) {
        for (const auto& page : pages) {
            SCOPED_TRACE(page.path);
            ui->SetBackdropBlurQuality(initial);
            ui->RequestLoadDocument(page.path);
            ui->Update();
            auto* document = ui->GetContext()->GetDocument(page.id);
            ASSERT_NE(document, nullptr);
            const auto check = [&](UiBlurQuality quality) {
                const float scale = quality == UiBlurQuality::Off    ? 0.0f
                                    : quality == UiBlurQuality::Half ? 0.5f
                                                                     : 1.0f;
                for (const auto& spec : page.elements) {
                    SCOPED_TRACE(spec.selector);
                    ASSERT_NO_FATAL_FAILURE(ExpectBackdrop(document->QuerySelector(spec.selector),
                                                           spec.full_radius * scale));
                }
            };
            ASSERT_NO_FATAL_FAILURE(check(initial));
            for (const auto quality : {UiBlurQuality::Half,
                                       UiBlurQuality::Half,
                                       UiBlurQuality::Off,
                                       UiBlurQuality::Full,
                                       UiBlurQuality::Half,
                                       UiBlurQuality::Full}) {
                SCOPED_TRACE(static_cast<int>(quality));
                ui->SetBackdropBlurQuality(quality);
                ui->Update();
                ASSERT_NO_FATAL_FAILURE(check(quality));
            }
        }
    }
}
} // namespace
