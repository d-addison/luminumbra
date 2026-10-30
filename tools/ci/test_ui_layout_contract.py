"""GL-free wiring checks for the capture-only UI layout sidecar."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]


def source(name):
    return (ROOT / name).read_text(encoding="utf-8")


class UiLayoutContract(unittest.TestCase):
    def test_capture_validates_before_payload_and_writes_metadata_last(self):
        menu = source("src/luminumbra_client/app/MenuScreens.cpp")
        helper = menu.split("bool WriteUiScreenshot(", 1)[1].split("} // namespace", 1)[0]
        stages = [helper.index(token) for token in (
            "UI::CollectUiLayout(", "UI::SerializeUiLayout(layout)",
            'sidecar.replace_extension(".layout.json")',
            "WritePixelBufferPpm(shot, width, height)",
            "Luminumbra::Rendering::WriteCaptureText(sidecar, bytes)")]
        self.assertEqual(stages, sorted(stages))
        capture = menu.split("if (ready_to_capture) {", 2)[-1]
        self.assertLess(capture.index("if (!WriteUiScreenshot("),
                        capture.index("++app.capture.ui_screen_index"))
        self.assertIn('fail_capture("screenshot output failed")', capture)
        mode = menu.index("if (!thumbnails_pending && !app.capture.ui_screens.empty()")
        self.assertGreater(menu.index("if (!WriteUiScreenshot("), mode)
        self.assertEqual(menu.count("WriteUiScreenshot("), 2)

    def test_menu_capture_excludes_imgui_before_its_composite(self):
        menu = source("src/luminumbra_client/app/MenuScreens.cpp")
        helper = menu.split("bool WriteUiScreenshot(", 1)[1].split("} // namespace", 1)[0]
        self.assertRegex(
            helper,
            r"UI::CollectUiLayout\(\s*uiManager \? uiManager->GetContext\(\) : nullptr,\s*"
            r"nullptr,\s*screen,\s*width,\s*height\)")
        self.assertLess(menu.index("uiManager->Render();"),
                        menu.index("if (!WriteUiScreenshot("))
        self.assertIn("glReadBuffer(GL_BACK);", menu)

        client = source("src/luminumbra_client/main_client.cpp")
        stages = [client.index(token) for token in (
            "ImGui::NewFrame();", "RenderMenuScreens(g_app,", "ImGui::Render();",
            "ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());")]
        self.assertEqual(stages, sorted(stages))
        self.assertEqual(client.count("RenderMenuScreens(g_app,"), 1)
        self.assertNotIn("UI::CollectUiLayout(", client)
        self.assertNotIn("UI::SerializeUiLayout(", client)

    def test_sources_and_always_running_serializer_are_registered(self):
        registered = source("src/luminumbra_client/sources.cmake")
        for name in ("CollectUiLayout.cpp", "UiLayout.cpp"):
            self.assertEqual(registered.count("/ui/" + name), 1)
        cmake = source("test/CMakeLists.txt")
        self.assertRegex(cmake, r'add_executable\(ui_layout_test\s+ui/ui_layout_test.cpp\s+'
                         r'../src/luminumbra_client/ui/UiLayout.cpp\s*\)')
        targets = re.search(r'^set\(LUMINUMBRA_GTEST_TARGETS\s+(.*?)^\)',
                            cmake, re.S | re.M)
        self.assertIsNotNone(targets)
        self.assertEqual(targets[1].split().count("ui_layout_test"), 1)
        pure = source("test/ui/ui_layout_test.cpp")
        self.assertNotIn("GTEST_SKIP", pure)
        self.assertNotIn("glfw", pure.lower())
        self.assertIn("ExactSchemaAndBytesAreStableWithoutGl", pure)
        self.assertIn("NAME UiLayoutSourceContract", source("tools/CMakeLists.txt"))
        self.assertNotIn("NAME UiLayoutSourceContract", cmake)
        self.assertIn('"tools/ci/test_ui_layout_contract.py"', source("tools/ci/ci_paths.py"))

    def test_collector_uses_pinned_geometry_and_never_guesses_paint_order(self):
        collector = source("src/luminumbra_client/ui/CollectUiLayout.cpp")
        for token in ("GetNumDocuments()", "GetDocument(index)", "IsVisible()",
                      "GetAbsoluteOffset(Rml::BoxArea::Border)",
                      "GetBox().GetSize(Rml::BoxArea::Border)",
                      "GetClippingRegion(&element, clip, &masks)",
                      "imgui->Windows", "window->Collapsed", "!window->Hidden"):
            self.assertIn(token, collector)
        self.assertIn(".z = std::nullopt", collector)
        for forbidden in ("GetInnerRML(", "glReadPixels(", "world_hash", "GameSession"):
            self.assertNotIn(forbidden, collector)
        self.assertIn("UI layout sidecars", source("docs/visual-regression.md"))


if __name__ == "__main__":
    unittest.main()
