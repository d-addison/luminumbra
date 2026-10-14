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
} // namespace

TEST(LoadingOverlay, NoContextDrawsNothing) {
    ScopedNullImGuiContext no_context;
    ASSERT_EQ(ImGui::GetCurrentContext(), nullptr);
    EXPECT_FALSE(DrawLoadingOverlay("x", 0.5f, 1, 2));
}

TEST(LoadingOverlay, WithContextBuildsOneWindow) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    ImGui::SetCurrentContext(context);
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
    ImGui::EndFrame();
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
}
