#include "luminumbra_client/rendering/WorldLoadingVisualizer.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

namespace {
using Luminumbra::Client::DrawLoadingOverlay;

// Saves the current ImGui context, clears it for the scope, and restores it on exit.
class ScopedNullImGuiContext {
public:
    ScopedNullImGuiContext()
        : m_previous(ImGui::GetCurrentContext()) {
        ImGui::SetCurrentContext(nullptr);
    }
    ~ScopedNullImGuiContext() {
        ImGui::SetCurrentContext(m_previous);
    }
    ScopedNullImGuiContext(const ScopedNullImGuiContext&) = delete;
    ScopedNullImGuiContext& operator=(const ScopedNullImGuiContext&) = delete;

private:
    ImGuiContext* m_previous;
};

// Owns the test context and closes any frame before restoring the caller's context.
class ScopedImGuiContext {
public:
    ScopedImGuiContext()
        : m_previous(ImGui::GetCurrentContext())
        , m_context(ImGui::CreateContext()) {
        ImGui::SetCurrentContext(m_context);
    }
    ~ScopedImGuiContext() {
        ImGui::SetCurrentContext(m_context);
        if (m_context->WithinFrameScope)
            ImGui::EndFrame();
        ImGui::DestroyContext(m_context);
        ImGui::SetCurrentContext(m_previous);
    }
    ScopedImGuiContext(const ScopedImGuiContext&) = delete;
    ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;

private:
    ImGuiContext* m_previous;
    ImGuiContext* m_context;
};
} // namespace

TEST(LoadingOverlay, NoContextDrawsNothing) {
    ScopedNullImGuiContext no_context;
    ASSERT_EQ(ImGui::GetCurrentContext(), nullptr);
    EXPECT_FALSE(DrawLoadingOverlay("x", 0.5f, 1, 2));
}

TEST(LoadingOverlay, WithContextBuildsOneWindow) {
    ScopedImGuiContext context;
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280, 720);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    ImGui::NewFrame();
    EXPECT_TRUE(DrawLoadingOverlay("Loading", 0.25f, 3, 10));
    ImGuiWindow* window = ImGui::FindWindowByName("Loading");
    ASSERT_NE(window, nullptr);
    EXPECT_TRUE(window->Active);
}
