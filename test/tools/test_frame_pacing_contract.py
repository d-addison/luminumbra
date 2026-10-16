"""Source contracts for presentation wiring and frame-work timing boundaries."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[2]
CLIENT = ROOT / "src/luminumbra_client"


def compact_cpp(source):
    # Preserve quoted strings while removing comments, so examples in comments
    # cannot satisfy the contract. Normalize whitespace to tolerate formatting.
    tokens = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
                        re.DOTALL)
    source = tokens.sub(lambda match: "" if match[0].startswith(("//", "/*"))
                        else match[0], source)
    return re.sub(r"\s+", "", source)


def read_cpp(path):
    return compact_cpp((CLIENT / path).read_text(encoding="utf-8"))


RESOLVE = (
    "luminumbra::core::ResolveFramePacing("
    "g_systemConfig.user().vsync,g_systemConfig.user().frame_cap,pacing_overrides)"
)
WAIT = "frame_limiter.pace(pacing.frame_cap,frame_timing);"
LOOP = "while(!glfwWindowShouldClose(window)){"


class FramePacingSourceContract(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.main = read_cpp("main_client.cpp")
        cls.debug_ui = read_cpp("app/DebugOverlays.cpp")
        cls.rml_ui = read_cpp("ui/Rml_UIManager.cpp")

    def assert_measurement_wiring(self, source):
        self.assertIn(
            "constluminumbra::core::FramePacingOverridespacing_overrides{"
            ".benchmark_active=!g_app.capture.render_benchmark_path.empty(),"
            ".scenario_active=scenario_config.active(),"
            ".profiling_active=g_app.capture.profile_fly_seconds>0.0};", source)
        # Startup, loop-top cap selection and post-UI present must all resolve
        # live player choices with the same measurement override.
        self.assertEqual(source.count("luminumbra::core::ResolveFramePacing("), 3)
        self.assertEqual(source.count(RESOLVE), 3)
        self.assertIn("intapplied_swap_interval=" + RESOLVE + ".vsync?1:0;"
                      "glfwSwapInterval(applied_swap_interval);", source)
        self.assertIn("constautopacing=" + RESOLVE + ";", source)
        self.assertIn("constintswap_interval=" + RESOLVE + ".vsync?1:0;", source)

    def assert_wait_precedes_timers(self, source):
        loop = source[source.index(LOOP):]
        self.assertEqual(source.count(WAIT), 1)
        wait = loop.index(WAIT)
        self.assertLess(loop.index("constautopacing=" + RESOLVE + ";"), wait)
        self.assertIn("Luminumbra::Client::FrameLimiterframe_limiter;", source[:source.index(LOOP)])
        self.assertIn("Luminumbra::Client::SteadyFrameTimingframe_timing;", source[:source.index(LOOP)])
        # Guard every work clock, including the first clock read: moving the
        # limiter into any CPU, GPU, frame-wall or SLOWFRAME bucket must fail.
        for marker in (
            "floatcurrentFrame=(float)glfwGetTime();",
            "std::chrono::steady_clock::now()",
            "g_rb_frame_start=std::chrono::steady_clock::now();",
            "benchmark_queries->begin_frame(measured_frame_id);",
            "constauto_frameStart=std::chrono::steady_clock::now();",
            "constauto_rb_poll_t0=std::chrono::steady_clock::now();",
        ):
            self.assertLess(wait, loop.index(marker), marker)
        self.assertIn("(_now-_frameStart).count()", loop)
        self.assertIn("(g_rb_before_swap-g_rb_frame_start)", loop)

    def assert_live_settings_wiring(self, main, debug_ui, rml_ui):
        self.assertNotIn("glfwSwapInterval(", debug_ui)
        self.assertNotIn("glfwSwapInterval(", rml_ui)
        self.assertEqual(main.count("glfwSwapInterval("), 2)
        self.assertIn("sb.SetVSync=[&](boolv){g_systemConfig.user().vsync=v;};", main)
        self.assertIn("sb.SetFrameCap=[&](intcap){"
                      "if(luminumbra::core::IsValidFrameCap(cap))"
                      "g_systemConfig.user().frame_cap=cap;};", main)
        self.assertIn('ImGui::Checkbox("VSync",&us.vsync);', debug_ui)
        self.assertIn("us.frame_cap=luminumbra::core::kFrameCaps[selected_cap];", debug_ui)
        self.assertIn('b.SetVSync(value=="on"||value=="1"||value=="true");', rml_ui)
        self.assertIn('elseif(id=="setting_frame_cap")', rml_ui)
        for cap in (0, 60, 120, 144, 240):
            self.assertIn(f'if(value=="{cap}")b.SetFrameCap({cap});', rml_ui)

        loop = main[main.index(LOOP):]
        resolve = loop.index("constintswap_interval=" + RESOLVE)
        self.assertLess(loop.index("g_uiManager->Update();"), resolve)
        self.assertLess(loop.index("DrawSettingsWindow("), resolve)
        apply = "if(swap_interval!=applied_swap_interval){glfwSwapInterval(swap_interval);"
        self.assertIn(apply + "applied_swap_interval=swap_interval;}", loop)
        self.assertLess(resolve, loop.index(apply))
        self.assertLess(loop.index(apply), loop.index("glfwSwapBuffers(window);"))

    def test_all_measurement_overrides_reach_all_production_calls(self):
        self.assert_measurement_wiring(self.main)

    def test_wait_is_excluded_from_work_timers(self):
        self.assert_wait_precedes_timers(self.main)

    def test_live_settings_use_the_overridden_present_path(self):
        self.assert_live_settings_wiring(self.main, self.debug_ui, self.rml_ui)

    def test_each_missing_call_site_override_is_detected(self):
        starts = [match.start() for match in re.finditer(re.escape(RESOLVE), self.main)]
        self.assertEqual(len(starts), 3)
        for start in starts:
            with self.subTest(call=start):
                changed = (self.main[:start] + RESOLVE.replace(
                    ",pacing_overrides)", ")") + self.main[start + len(RESOLVE):])
                with self.assertRaises(AssertionError):
                    self.assert_measurement_wiring(changed)

    def test_each_disabled_measurement_mode_is_detected(self):
        for field, value in (
            ("benchmark_active", "!g_app.capture.render_benchmark_path.empty()"),
            ("scenario_active", "scenario_config.active()"),
            ("profiling_active", "g_app.capture.profile_fly_seconds>0.0"),
        ):
            with self.subTest(mode=field):
                original = f".{field}={value}"
                self.assertIn(original, self.main)
                changed = self.main.replace(original, f".{field}=false", 1)
                with self.assertRaises(AssertionError):
                    self.assert_measurement_wiring(changed)

    def test_wait_inside_work_timers_is_detected(self):
        for marker in (
            "g_rb_frame_start=std::chrono::steady_clock::now();",
            "benchmark_queries->begin_frame(measured_frame_id);",
            "constauto_frameStart=std::chrono::steady_clock::now();",
        ):
            with self.subTest(timer=marker):
                self.assertIn(marker, self.main)
                changed = self.main.replace(WAIT, "").replace(marker, marker + WAIT, 1)
                with self.assertRaises(AssertionError):
                    self.assert_wait_precedes_timers(changed)

    def test_direct_swap_interval_calls_in_either_ui_are_detected(self):
        for direct_ui in ("debug", "rml"):
            with self.subTest(ui=direct_ui):
                debug_ui, rml_ui = self.debug_ui, self.rml_ui
                if direct_ui == "debug":
                    debug_ui += "glfwSwapInterval(us.vsync?1:0);"
                else:
                    rml_ui += "glfwSwapInterval(value?1:0);"
                with self.assertRaises(AssertionError):
                    self.assert_live_settings_wiring(self.main, debug_ui, rml_ui)


if __name__ == "__main__":
    unittest.main()
