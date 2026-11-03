"""Authored backdrop coverage and settings wiring for issue #393; no native runtime."""

from pathlib import Path
import re
import unittest
import xml.etree.ElementTree as ET


ROOT = Path(__file__).resolve().parents[2]


def compact_cpp(path):
    source = path.read_text(encoding="utf-8")
    tokens = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)
    source = tokens.sub(lambda match: "" if match[0].startswith(("//", "/*"))
                        else match[0], source)
    return re.sub(r"\s+", "", source)


def rules(path):
    text = re.sub(r"/\*.*?\*/", "", path.read_text(encoding="utf-8"), flags=re.S)
    return [
        ([selector.strip() for selector in selectors.split(",")], body)
        for selectors, body in re.findall(r"([^{}]+)\{([^{}]*)\}", text)
    ]


class GraphicsUiContract(unittest.TestCase):
    def test_new_presets_are_conservative_and_saved_revisions_stay_explicit(self):
        header = compact_cpp(ROOT / "src/luminumbra_common/core/GraphicsPresets.h")
        resolver = compact_cpp(ROOT / "src/luminumbra_common/core/GraphicsPresets.cpp")
        loader = compact_cpp(ROOT / "src/luminumbra_common/core/SystemConfig.cpp")
        self.assertIn("kGraphicsPresetRevision=2;", header)
        self.assertIn("constexprstd::arraykRevision2{kUltraRevision1,kUltraRevision1,"
                      "kUltraRevision1,kUltraRevision1};", resolver)
        self.assertIn("settings.preset_revision==1?kRevision1[index]:kRevision2[index]", resolver)
        self.assertIn('parsed.preset_revision=graphics["preset_revision"].get<std::uint32_t>();', loader)
        self.assertIn('(graphics["preset_revision"]!=1&&'
                      'graphics["preset_revision"]!=kGraphicsPresetRevision)', loader)

    def test_frame_pacing_choices_reach_resolved_graphics_and_persistence(self):
        header = compact_cpp(ROOT / "src/luminumbra_common/core/GraphicsPresets.h")
        resolver = compact_cpp(ROOT / "src/luminumbra_common/core/GraphicsPresets.cpp")
        loader = compact_cpp(ROOT / "src/luminumbra_common/core/SystemConfig.cpp")
        main = compact_cpp(ROOT / "src/luminumbra_client/main_client.cpp")
        self.assertIn("FramePacingframe_pacing{.vsync=true,.frame_cap=0};", header)
        self.assertIn("if(!IsValidFrameCap(frame_pacing.frame_cap)){", resolver)
        self.assertLess(resolver.index("if(!IsValidFrameCap(frame_pacing.frame_cap)){"),
                        resolver.index("result=std::move(resolved);"))
        self.assertIn(".frame_pacing=frame_pacing,", resolver)
        self.assertIn('{"vsync",resolved.frame_pacing.vsync}', loader)
        self.assertIn('{"frame_cap",resolved.frame_pacing.frame_cap}', loader)
        pacing = ("{.vsync=g_systemConfig.user().vsync,"
                  ".frame_cap=g_systemConfig.user().frame_cap}")
        self.assertIn("resolved_graphics,graphics_error," + pacing + ");", main)
        self.assertIn("candidate,graphics_error," + pacing + ")&&", main)
        self.assertIn("display.effective.frame_pacing=" + pacing + ";", main)

    def test_first_launch_does_not_save_after_renderer_apply_failure(self):
        main = compact_cpp(ROOT / "src/luminumbra_client/main_client.cpp")
        self.assertIn("graphics_ready=renderPipeline.apply_graphics(resolved_graphics);", main)
        self.assertIn("if(graphics_ready&&(graphics_first_launch||graphics_detect_requested))"
                      "graphics_state.ScheduleSave(glfwGetTime());", main)

    def test_every_authored_backdrop_has_off_and_half_rules(self):
        authored = 0
        for name in ("game_theme.rcss", "pages.rcss"):
            entries = rules(ROOT / "data/ui" / name)
            quality = {}
            for selectors, body in entries:
                match = re.search(r"backdrop-filter\s*:\s*([^;]+);", body)
                if match:
                    for selector in selectors:
                        quality[selector] = match[1].strip()
            expected = ({"button": 12, ".settings-panel": 22, ".list-item": 14,
                         ".creation-panel": 22} if name == "game_theme.rcss" else
                        {".pause-overlay": 10, ".pause-panel": 10})
            for selector, radius in expected.items():
                self.assertEqual(quality[selector], f"blur({radius}px)")
            for selector, value in quality.items():
                if "backdrop-" in selector or value == "none":
                    continue
                with self.subTest(sheet=name, selector=selector):
                    radius = re.fullmatch(r"blur\((\d+(?:\.\d+)?)px\)", value)
                    self.assertIsNotNone(radius)
                    authored += 1
                    expected = float(radius.group(1)) / 2
                    self.assertEqual(quality[f"body.backdrop-half {selector}"], f"blur({expected:g}px)")
                    self.assertEqual(quality[f"body.backdrop-off {selector}"], "none")
            self.assertFalse(any("backdrop-full" in selector for selector in quality))
        self.assertEqual(authored, 6)

    def test_preset_targets_ranges_and_enum_choices(self):
        body = ET.parse(ROOT / "data/ui/settings.rml").getroot()
        controls = {element.get("id"): element for element in body.iter() if element.get("id")}
        self.assertEqual([option.get("value") for option in controls["graphics_preset"]],
                         ["low", "medium", "high", "ultra"])
        numeric = {
            "render_scale": (0.5, 1.0), "grass_density": (0.1, 2.0),
            "grass_fade_start_m": (0.0, 160.0), "grass_fade_end_m": (1.0, 160.0),
            "far_terrain_radius_m": (768.0, 3000.0),
        }
        for key, bounds in numeric.items():
            with self.subTest(key=key):
                element = controls[f"graphics_{key}"]
                self.assertEqual((float(element.get("min")), float(element.get("max"))), bounds)
                self.assertGreater(float(element.get("step")), 0)
                self.assertEqual(element.get("data-key"), key)
        for key, choices in {
            "shadow_resolution": ["1024", "1536", "2048"],
            "volumetric_quality": ["0", "1"], "ui_blur": ["off", "half", "full"],
            "grass_enabled": ["on", "off"],
        }.items():
            self.assertEqual([option.get("value") for option in controls[f"graphics_{key}"]], choices)
        for key in ("graphics_state", "graphics_hardware", "graphics_status", "graphics_reset",
                    "graphics_detect", "graphics_retry_save"):
            self.assertIn(key, controls)

    def test_only_first_launch_or_explicit_action_selects_detection_at_startup(self):
        main = (ROOT / "src/luminumbra_client/main_client.cpp").read_text(encoding="utf-8")
        detection = main.index("g_app.hardwareInfo = Luminumbra::Client::DetectHardwareInfo(window)")
        target_startup = main.index("renderPipeline.startup(", detection)
        startup = main[detection:target_startup]
        self.assertIn('HasCommandLineFlag(argc, argv, "--detect-and-apply-graphics")', startup)
        self.assertIn("g_systemConfig.settings_load_report().needs_first_launch_detection", startup)
        self.assertIn("if (graphics_first_launch || graphics_detect_requested)", startup)

    def test_explicit_saves_share_graphics_and_developer_transaction(self):
        main = (ROOT / "src/luminumbra_client/main_client.cpp").read_text(encoding="utf-8")
        bridge = main.split("sb.Save = [&] {", 1)[1].split("};", 1)[0]
        self.assertEqual(bridge.strip(), "return developerConsole.SavePreferences();")
        host = main.split("developerHost.save = [&] {", 1)[1].split(
            "developerHost.gameplay_help", 1)[0]
        self.assertRegex(host, r"if \(!apply_pending_graphics\(\)\)\s+return false;")
        ordered = [
            "if (!apply_pending_graphics())", "auto& preferences",
            "preferences.widgets[", "preferences.layouts[", "preferences.bindings[",
            "return save_graphics();",
        ]
        positions = [host.index(fragment) for fragment in ordered]
        self.assertEqual(positions, sorted(positions))
        writer = main.split("const auto save_graphics = [&] {", 1)[1].split("};", 1)[0]
        self.assertIn("graphics_state.Saved(saved);", writer)
        self.assertIn("return saved;", writer)
        # Every overlay writer in the client must participate in the shared transaction.
        writers = []
        for path in sorted((ROOT / "src/luminumbra_client").rglob("*.cpp")):
            for line in path.read_text(encoding="utf-8").splitlines():
                if ".SaveUserOverlay(" in line:
                    writers.append(path.name)
        self.assertEqual(writers, ["main_client.cpp"])
        overlays = (ROOT / "src/luminumbra_client/app/DebugOverlays.cpp").read_text(encoding="utf-8")
        settings_save = overlays.split('ImGui::Button("Save settings")', 1)[1].split("}", 1)[0]
        self.assertIn("const bool ok = console.SavePreferences();", settings_save)


if __name__ == "__main__":
    unittest.main()
