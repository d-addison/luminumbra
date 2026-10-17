"""Guard production attribution wiring beyond the pure bucket/count tests."""

from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
CLIENT = ROOT / "src/luminumbra_client"


def read(path):
    return path.read_text(encoding="utf-8")


def compact(text):
    return re.sub(r"\s+", " ", text).strip()


def block(text, marker):
    """Extract balanced C++ braces, ignoring comments and string literals."""
    masked = re.sub(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: " " * len(match.group()), text, flags=re.S,
    )
    start = masked.index("{", text.index(marker))
    depth = 1
    for index in range(start + 1, len(masked)):
        if masked[index] == "{":
            depth += 1
        elif masked[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:index]
    raise AssertionError(f"Unclosed block: {marker}")


class FrameAttributionWiring(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.main = read(CLIENT / "main_client.cpp")
        cls.foliage = read(CLIENT / "rendering/passes/FoliagePass.cpp")
        cls.pass_header = read(CLIENT / "rendering/passes/FoliagePass.h")
        cls.header = read(CLIENT / "app/FrameAttribution.h")
        cls.collector = read(CLIENT / "app/FrameAttribution.cpp")

    def test_render_bucket_receives_exclusive_time_and_feeds_outputs(self):
        main = compact(self.main)
        self.assertIn("rb_render_bucket_ms = Attribution::exclusive_parent_ms("
                      "rb_render_call_ms, render_foliage_before, foliage_scope.snapshot().rebuild_ms);",
                      main)
        self.assertIn(".render = rb_render_bucket_ms", main)
        self.assertRegex(main, r"rb_stream_ms, rb_render_bucket_ms, _presentMs, _other,")
        self.assertIn("rb_render_call += rb_render_call_ms;", main)

    def test_collection_and_split_log_require_explicit_opt_in(self):
        main = compact(self.main)
        self.assertIn('frame_csv_enabled = HasCommandLineFlag(argc, argv, "--frame-attribution-csv")', main)
        self.assertIn('frame_attribution_enabled = frame_csv_enabled || '
                      'HasCommandLineFlag(argc, argv, "--frame-attribution");', main)
        self.assertIn("FoliageFrameScope foliage_scope(frame_attribution_enabled);", main)
        self.assertIn("StageTimer menu_timer(rb_menu_ms, frame_attribution_enabled);", main)
        self.assertRegex(main, r'if \(frame_attribution_enabled\) LUMINUMBRA_CORE_WARN\(\s*"\s*foliage-split:')
        self.assertIn("explicit FoliageFrameScope(bool enabled = false);", self.header)
        self.assertIn("current_foliage_frame() = enabled ? &m_frame : nullptr;", self.collector)
        self.assertRegex(compact(self.foliage),
                         r"if \(collect_attribution\) \{ // Diagnostic[^\n]*? "
                         r"Attribution::ScatterSignature components\{\};")

    def test_menu_input_ownership_and_exclusive_attribution_share_the_call(self):
        main = compact(self.main)
        self.assertRegex(main, r"RenderMenuScreens\([^;]*menuPreviewState, uiInputRouter\); "
                         r"menu_timer.stop\(\); rb_menu_ms = Attribution::exclusive_parent_ms\("
                         r" rb_menu_ms, menu_foliage_before, foliage_scope.snapshot\(\).rebuild_ms\);")
        menu = compact(block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens("))
        self.assertTrue(menu.startswith(
            "const bool previewMouseAllowed = previewState.UpdateInputOwnership(inputRouter);"))

    def test_disabled_timers_do_not_read_clocks(self):
        constructor = compact(block(self.header, "explicit StageTimer("))
        self.assertEqual(constructor, "if (m_running) m_start = std::chrono::steady_clock::now();")
        self.assertIn("m_running(enabled)", self.header)
        self.assertIn("time_point m_start{};", self.header)
        timer = block(self.header, "class StageTimer")
        self.assertEqual(timer.count("steady_clock::now()"), 2)
        self.assertTrue(compact(block(timer, "void stop()")).startswith("if (m_running) {"))
        scope = block(self.foliage, "class RebuildAttributionScope")
        self.assertIn("if (m_enabled) m_start = std::chrono::steady_clock::now();", compact(scope))
        self.assertTrue(compact(block(scope, "~RebuildAttributionScope()")).startswith(
            "if (!m_enabled) return;"))
        self.assertEqual(self.foliage.count("steady_clock::now()"), 2)
        timers = re.findall(r"Attribution::StageTimer \w+\((.*?)\);", self.foliage, re.S)
        self.assertGreater(len(timers), 10)
        for arguments in timers:
            self.assertTrue(compact(arguments).endswith(", collect_attribution"))

    def test_water_subzones_share_the_opt_in_frame_and_csv(self):
        main = compact(self.main)
        self.assertIn("WaterAttribution::FrameScope water_scope(frame_attribution_enabled);", main)
        self.assertIn("foliage_frame.count_generation, water_scope.snapshot())", main)
        self.assertRegex(main, r'if \(frame_attribution_enabled\) \{ '
                              r'const auto& water = water_scope.snapshot\(\); '
                              r'LUMINUMBRA_CORE_WARN\( " water-split:')
        for column in ("water_seed_inputs_ms", "water_initialization_ms", "water_sim_ms",
                       "water_seam_ms", "water_bookkeeping_ms", "water_fallback_columns"):
            self.assertIn(column, self.collector)

    def test_both_frozen_entry_points_are_counted_once(self):
        rebuild = block(self.foliage, "void FoliagePass::rebuild_instances(")
        qualification = block(self.foliage, "bool FoliagePass::begin_qualification()")
        frozen = block(self.foliage, "void FoliagePass::rebuild_frozen_inputs()")
        for caller in (rebuild, qualification):
            self.assertEqual(caller.count("RebuildAttributionScope rebuild_scope(*this);"), 1)
            self.assertEqual(caller.count("rebuild_frozen_inputs();"), 1)
        self.assertNotIn("RebuildAttributionScope", frozen)
        self.assertNotIn("m_rebuild_attribution.dispatch_ms", rebuild)
        self.assertIn("m_rebuild_attribution = {};", qualification)
        self.assertIn("m_rebuild_attribution.reasons = Attribution::Initial;", qualification)
        self.assertIn("m_rebuild_attribution.path = Attribution::FoliagePath::FrozenGpu;", frozen)
        timer_position = frozen.index("Attribution::StageTimer timer(")
        for operation in ("glClearBufferData(", "dispatch_scatter();", "submit_foliage_readback();"):
            self.assertGreater(frozen.index(operation), timer_position)
        self.assertNotIn("timer.stop()", frozen)

    def test_client_uses_only_the_opt_in_profiler_interface(self):
        targets = read(ROOT / "src/CMakeLists.txt")
        links = re.search(r"target_link_libraries\(luminumbra_client\s+(.*?)\)", targets, re.S)
        self.assertIsNotNone(links)
        self.assertIn("luminumbra_profiler", links.group(1))
        self.assertNotIn("TracyClient", links.group(1))
        tracy = read(ROOT / "cmake/tracy.cmake")
        self.assertRegex(tracy, r'option\(LUMINUMBRA_ENABLE_TRACY\s+"[^"\n]*" OFF\)')
        before, gated = tracy.split("if(LUMINUMBRA_ENABLE_TRACY)", 1)
        enabled = gated.split("else()", 1)[0]
        self.assertIn("FetchContent_MakeAvailable(tracy)", enabled)
        self.assertIn("target_link_libraries(luminumbra_profiler INTERFACE TracyClient)", enabled)
        self.assertNotIn("FetchContent", before.split("option(", 1)[1])

    def test_public_counts_use_readback_values_and_publish_unknown(self):
        for accessor, result in (("bool frame_instance_count_known()", ".has_value()"),
                                 ("std::size_t frame_instance_count()", ".value_or(0)")):
            body = compact(block(self.pass_header, accessor))
            self.assertIn("generated_instance_count( m_gpu_active, m_readback_enabled, "
                          "m_gpu_generated_count.has_value(), m_gpu_active ? "
                          "m_gpu_generated_count.value_or(0) : m_draw_instance_limit)", body)
            self.assertTrue(body.endswith(result + ";"))
        readback = block(self.foliage, "void FoliagePass::poll_foliage_readback()")
        self.assertIn("m_gpu_generated_count = count;", readback)
        self.assertIn("m_gpu_count_generation = generation;", readback)
        gpu = block(self.foliage, "bool FoliagePass::rebuild_instances_gpu(")
        self.assertIn("m_gpu_generated_count.reset();", gpu)
        scan = compact(read(CLIENT / "rendering/FrameScan.cpp"))
        self.assertIn("fol->frame_instance_count_known()", scan)
        self.assertIn('foliage["instances"] = report.foliage_instance_count_known ? '
                      'nlohmann::json(report.foliage_instances) : nlohmann::json(nullptr);', scan)
        pipeline = compact(block(read(CLIENT / "rendering/RenderPipeline.cpp"),
                                 "void RenderPipeline::execute_stage_foliage("))
        self.assertIn("!m_foliage_pass->frame_instance_count_known() && "
                      "m_foliage_pass->last_draw_calls() > 0", pipeline)
        self.assertIn("m_last_render_pass_stats.foliage_instance_count_known = "
                      "m_foliage_pass->frame_instance_count_known();", pipeline)
        overlay = compact(block(read(CLIENT / "app/DebugOverlays.cpp"),
                                "void DrawGpuProfilerContents("))
        self.assertNotIn("ImGui::Begin(", overlay)
        self.assertNotIn("ImGui::End(", overlay)
        self.assertIn('if (gp.foliage_instance_count_known) ImGui::Text(', overlay)
        self.assertIn('capacity (count unavailable)', overlay)
        main = compact(self.main)
        self.assertIn("foliage_frame.capacity, foliage_frame.count_known, "
                      "foliage_frame.generated_count, foliage_frame.count_generation, "
                      "water_scope.snapshot())", main)
        scope = compact(block(self.foliage, "~RebuildAttributionScope()"))
        self.assertIn("m_pass.frame_instance_count_known() ? "
                      "std::optional<std::size_t>(m_pass.frame_instance_count()) : std::nullopt", scope)

    def test_nullable_frame_scan_count_requires_v2_for_all_capture_paths(self):
        writer = compact(block(read(CLIENT / "rendering/FrameScan.cpp"),
                               "bool WriteFrameScanReport("))
        self.assertIn('j["schema"] = "luminumbra.frame_scan.v2";', writer)
        self.assertIn('foliage["instances"] = report.foliage_instance_count_known ? '
                      'nlohmann::json(report.foliage_instances) : nlohmann::json(nullptr);', writer)
        self.assertIn('foliage["instance_capacity"] = report.foliage_instance_capacity;', writer)
        self.assertIn('j["foliage"] = std::move(foliage);', writer)
        for source in (self.main, read(CLIENT / "rendering/SceneSurvey.cpp")):
            self.assertIn("WriteFrameScanReport(", source)
            self.assertNotIn("luminumbra.frame_scan.", source)

    def test_recorder_propagates_writer_failure_and_checks_stream_state(self):
        record = compact(block(self.collector, "bool FrameRecorder::record("))
        self.assertTrue(record.startswith("if (!active()) return false; return write_csv_row(m_out,"))
        self.assertEqual(compact(block(self.header, "bool active() const")),
                         "return m_out.is_open() && m_out.good();")

    def test_recorder_finalization_failure_reaches_shutdown_error_path(self):
        finish = compact(block(self.collector, "bool FrameRecorder::finish()"))
        self.assertIn("return finalize_csv(m_out);", finish)
        failure = compact(block(self.main, "if (!frame_recorder.finish())"))
        self.assertIn('LUMINUMBRA_CORE_ERROR("Frame attribution CSV finalization failed");', failure)
        self.assertIn("if (exit_code == 0) exit_code = 1;", failure)
        self.assertLess(self.main.index("if (!frame_recorder.finish())"),
                        self.main.index('trace_shutdown("query_cleanup_started")'))
        self.assertIn("return exit_code;", self.main)


if __name__ == "__main__":
    unittest.main()
