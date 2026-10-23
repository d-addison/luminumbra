"""Guard production attribution wiring beyond the pure bucket/count tests."""

from pathlib import Path
import ast
import json
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


def cpp_expression(expression, **bindings):
    """Evaluate only scalar production predicates, without executing native code."""
    names = {}
    for index, (name, value) in enumerate(sorted(bindings.items(), key=lambda item: -len(item[0]))):
        token = f"value_{index}"
        expression = re.sub(r"(?<![\w.])" + re.escape(name) + r"(?![\w.])", token, expression)
        names[token] = value
    expression = expression.replace("&&", " and ").replace("||", " or ")
    expression = re.sub(r"!(?!=)", " not ", expression).replace("/", "//")
    expression = compact(re.sub(r"(?<=\d)'(?=\d)", "", expression))
    tree = ast.parse(expression, mode="eval")
    allowed = (ast.Expression, ast.BoolOp, ast.And, ast.Or, ast.UnaryOp, ast.Not,
               ast.BinOp, ast.Add, ast.Sub, ast.Mult, ast.FloorDiv, ast.Mod,
               ast.Compare, ast.Eq, ast.NotEq, ast.Lt, ast.LtE, ast.Gt, ast.GtE,
               ast.Name, ast.Load, ast.Constant)
    if any(not isinstance(node, allowed) for node in ast.walk(tree)):
        raise AssertionError(f"Unsupported scalar expression: {expression}")
    return eval(compile(tree, "<production predicate>", "eval"), {"__builtins__": {}}, names)


class CaptureGateEvaluation:
    """Interpret the shipping scalar gate; this does not execute C++ or GL."""

    def __init__(self):
        self.source = block(read(CLIENT / "app/MenuSceneSchedule.h"), "class MenuCaptureGate")
        self.state = {}
        for name, assigned, braced in re.findall(
                r"(?:Time|std::size_t|bool) (\w+)(?:\s*=\s*([^;]+)|\{([^}]*)\});",
                self.source[self.source.index("private:"):]):
            expression = assigned or braced or "0"
            self.state[name] = cpp_expression(expression, true=True, false=False)

    def call(self, method, **inputs):
        body = block(self.source, method)
        body = re.sub(r"std::chrono::seconds\{(\d+)\}", r"\1", body)
        statement = re.compile(
            r"if\s*\(([^()]*)\)\s*return\s+([^;]+);|"
            r"return\s+([^;]+);|(\w+)\s*=\s*([^;]+);"
        )
        position = 0
        for match in statement.finditer(body):
            if body[position:match.start()].strip():
                raise AssertionError("Unsupported production gate control flow")
            position = match.end()
            condition, guarded_return, returned, name, assigned = match.groups()
            values = dict(self.state, **inputs, true=True, false=False)
            if assigned is not None:
                self.state[name] = cpp_expression(assigned, **values)
            elif condition is None or cpp_expression(condition, **values):
                expression = guarded_return if condition is not None else returned
                if expression.startswith("Result::"):
                    return expression.removeprefix("Result::")
                return cpp_expression(expression, **values)
        if body[position:].strip():
            raise AssertionError("Incomplete production gate control flow")
        return None

    def arm(self, now=0, index=0, thumbnail=False):
        return self.call("bool arm(", now=now, index=index, thumbnail=thumbnail)

    def decide(self, now, settled=False, drawable=True, content_ready=True):
        return self.call("Result decide(", now=now, settled=settled,
                         drawable=drawable, content_ready=content_ready)


class MenuCaptureEvaluation:
    """Trace production capture predicates, including the pre-deadline path.

    This scalar policy evaluation supplies render/readiness observations. It is
    not a renderer, and provides no native screenshot qualification.
    """

    def __init__(self, live=False):
        source = read(CLIENT / "app/MenuScreens.cpp")
        self.menu = block(source, "void RenderMenuScreens(")
        self.gate = CaptureGateEvaluation() if "if (!PrepareMenuCapture(" in self.menu else None
        self.live = live
        self.count = 0
        self.start = None
        regular = re.sub(r"//[^\n]*", "", block(self.menu, "else if (app.capture.ui_screenshot_settle <"))
        self.admission = re.fullmatch(r"\s*if \((.*?)\)\s*"
                                      r"\+\+app.capture.ui_screenshot_settle;\s*", regular, re.S).group(1)
        self.required = int(re.search(r"else if \(app.capture.ui_screenshot_settle < (\d+)\)", self.menu).group(1))
        self.content = re.search(r"const bool content_ready\s*=\s*(.*?);", source, re.S)

    def frame(self, now, rendered=False, preview=True, ready=False,
              drawable=True, failed=False, document=True, index=0, source_failed=False,
              candidate_ready=None):
        if self.start is None:
            self.start = now
        if self.gate:
            if self.gate.arm(now, index=index):
                self.count = 0
                self.start = now
            if self.gate.decide(now) == "TimedOut":
                return "TimedOut"
        values = dict(scene_rendered=rendered, scenic=False, previewActive=preview)
        if self.live:
            live = block(self.menu, "if (preview_live_screen)")
            live = live[live.index("const bool world_ready"):]
            reset = re.search(r"if \((.*?)\)\s*app.capture.ui_preview_settle_after_ready = 0;", live, re.S).group(1)
            increment = re.search(r"else if \((.*?)\)\s*\+\+app.capture.ui_preview_settle_after_ready;", live, re.S).group(1)
            values["world_ready"] = ready
            if cpp_expression(reset, **values):
                self.count = 0
            elif cpp_expression(increment, **values):
                self.count += 1
            expressions = dict(re.findall(r"const bool (settled|timed_out) = (.*?);", live, re.S))
            constants = dict((name, float(number)) for name, number in re.findall(
                r"constexpr (?:double|int) (\w+) =\s*(\d+(?:\.\d+)?);", live))
            values.update(constants)
            values.update({"app.capture.ui_preview_settle_after_ready": self.count,
                           "waited_s": now - self.start})
            for name in ("settled", "timed_out"):
                values[name] = cpp_expression(expressions[name], **values)
            requested = cpp_expression(re.search(r"ready_to_capture = (.*?);", live, re.S).group(1), **values)
        else:
            requested = self.count >= self.required
            if not requested and cpp_expression(self.admission, **values):
                self.count += 1
        content_ready = True
        if self.content:
            content_ready = cpp_expression(self.content.group(1), **{
                "previewState.capture_document_ready": document,
                "app.menu.menu_backdrop_active": False, "scenic": False,
                "expects_preview": preview, "previewActive": preview,
                "worldgenPreview": preview, "worldgenPreview->world_ready()": ready,
                "worldgenPreview->candidate_ready()": ready if candidate_ready is None else candidate_ready,
                "worldgenPreview->last_build_failed()": failed,
                "previewState.candidate_source_failed": source_failed})
            if not content_ready or not drawable:
                self.count = 0
                requested = False
        if self.gate:
            return self.gate.decide(now, settled=requested, drawable=drawable,
                                    content_ready=content_ready)
        return "Ready" if requested else "Waiting"


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

    def test_slow_frame_keeps_far_reporting_with_exclusive_attribution(self):
        slow = compact(block(self.main, "if (_wallMs > 12.0"))
        self.assertIn("const auto slow_far = renderPipeline.far_world_reporting_state();", slow)
        self.assertIn("frame_attribution_enabled ? buckets.other(_wallMs, phase_scope.snapshot())", slow)
        self.assertRegex(slow, r"rb_stream_ms, rb_render_bucket_ms, _presentMs, _other,")
        for field in ("far_requested", "far_effective", "far_refusal", "far_suppressed",
                      "far_submission_refusal", "far_settled"):
            self.assertIn(field + "={}", slow)
        for value in ("FarWorldModeName(slow_far.selection.requested)",
                      "FarWorldModeName(slow_far.selection.effective)",
                      "FarWorldRefusalName(slow_far.selection.refusal)",
                      "FarWorldReportSuppressed(slow_far)",
                      "FarWorldSubmissionRefusalName( slow_far.submission_refusal)",
                      "slow_far.coverage_settled"):
            self.assertIn(value, slow)
        self.assertIn("foliage_frame.rebuild_ms, rb_menu_ms, rb_ui_ms,", slow)
        self.assertIn('"foliage={:.1f} menu={:.1f} ui={:.1f} | "', slow)
        self.assertIn("foliage-split:", slow)

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

    def test_benchmark_does_not_enable_foliage_input_hashing(self):
        update = compact(block(self.main, "static void UpdateFoliageScatter("))
        self.assertIn("foliage->set_evidence_enabled(scenario_config.foliage_visual_smoke() && "
                      "!g_app.capture.play_paths);", update)
        self.assertNotIn("render_benchmark_path", update)

    def test_menu_input_ownership_and_exclusive_attribution_share_the_call(self):
        main = compact(self.main)
        self.assertRegex(main, r"RenderMenuScreens\([^;]*menuPreviewState, uiInputRouter\); "
                         r"menu_timer.stop\(\); rb_menu_ms = Attribution::exclusive_parent_ms\("
                         r" rb_menu_ms, menu_foliage_before, foliage_scope.snapshot\(\).rebuild_ms\);")
        menu = compact(block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens("))
        self.assertTrue(menu.startswith(
            "const bool previewMouseAllowed = previewState.UpdateInputOwnership(inputRouter);"))

    def test_cached_menu_retains_input_ownership_and_scene_state(self):
        header = read(CLIENT / "app/MenuScreens.h")
        state = block(header, "struct MenuSceneState : MenuPreviewState")
        self.assertIn("MenuSceneSchedule schedule;", state)
        self.assertIn("MenuSceneImage scene_image;", state)
        self.assertIn("MenuSceneState& previewState,", header)
        self.assertIn("const UiInputRouter& inputRouter", header)
        self.assertIn("MenuSceneState menuPreviewState;", self.main)
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        self.assertLess(menu.index("UpdateInputOwnership(inputRouter)"),
                        menu.index("schedule.update(now, key, interactive)"))
        self.assertIn("previewMouseAllowed &&", menu)
        self.assertIn("interactive = true;", block(menu, "if (previewState.dragging)"))

    def test_poll_attribution_keeps_ownership_refresh_and_registered_overlays(self):
        poll = self.main[self.main.index("const auto _rb_poll_t0"):
                         self.main.index("glfwPollEvents();")]
        self.assertIn("poll_phases_before = Attribution::phase_total_ms();", poll)
        self.assertIn("poll_foliage_before = foliage_scope.snapshot().rebuild_ms;", poll)
        self.assertIn("RefreshInputOwnership(window);", poll)
        after_poll = compact(self.main[self.main.index("glfwPollEvents();"):])
        self.assertTrue(after_poll.startswith("glfwPollEvents(); RefreshInputOwnership(window);"))
        self.assertIn("developerConsole.Draw(developerFrame);", self.main)
        self.assertNotIn("DrawFrameStatusOverlays(", self.main)
        self.assertNotIn("DrawGpuProfilerOverlay(", self.main)
        self.assertRegex(compact(self.main),
                         r"menuPreviewState.schedule = \{\}; "
                         r"menuPreviewState.last_menu_frame = \{\}; "
                         r"//[^\n]*? if \(g_app.capture.timelapse_creatures\) "
                         r"DrawCreatureNameplates\(")

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

    def test_const_frame_scopes_allow_callback_collection(self):
        scope = block(self.header, "class FoliageFrameScope")
        self.assertIn("mutable FoliageFrame m_frame{};", scope)
        runtime = read(ROOT / "test/rendering/graphics_runtime_test.cpp")
        self.assertIn("const Attribution::FoliageFrameScope frame(true);", runtime)
        self.assertIn("return frame.snapshot().rebuild.reasons;", runtime)

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

    def test_foliage_cached_uniforms_retain_attributed_program_lifetimes(self):
        init = block(self.foliage, "void FoliagePass::init_compute(")
        destroy = block(self.foliage, "void FoliagePass::destroy_compute()")
        upload = block(self.foliage, "bool FoliagePass::rebuild_instances_gpu(")
        dispatch = block(self.foliage, "void FoliagePass::dispatch_scatter()")
        for program in ("m_compute_prog", "m_prefix_prog"):
            self.assertIn(f"AttributedGl::Program {program}{{}};", self.pass_header)
            self.assertIn(f"AttributedGl::DeleteProgram(glDeleteProgram, {program})", destroy)
            self.assertIn(f"{program} = {{}};", destroy)
        for cache, fields in (
            ("m_scatter_uniforms", ("chunk_count", "archetype_count", "total_weight",
                                     "max_candidates", "max_instances", "density_scale",
                                     "camera_xz", "fade_end_m", "wind_xz", "emit")),
            ("m_prefix_uniforms", ("group_count", "max_instances")),
        ):
            self.assertIn(f"{cache} = {{", init)
            self.assertIn(f"{cache} = {{}};", destroy)
            for field in fields:
                self.assertIn(f".{field} = glGetUniformLocation(", init)
                self.assertIn(f"{cache}.{field}", upload + dispatch)
        self.assertNotIn("glGetUniformLocation(", upload + dispatch)
        self.assertEqual(dispatch.count("AttributedGl::UseProgram("), 4)
        self.assertEqual(dispatch.count("AttributedGl::Submit("), 3)
        self.assertNotRegex(dispatch, r"\bgl(?:UseProgram|DispatchCompute)\s*\(")
        self.assertIn("m_uploaded_input_hash.reset();", destroy)
        self.assertIn("m_uploaded_chunk_count = 0;", destroy)
        self.assertIn("m_group_storage_bytes = 0;", destroy)

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
                      "foliage_frame.generated_count, foliage_frame.count_generation, phase_scope.snapshot(), foliage_frame.uploads)", main)
        scope = compact(block(self.foliage, "~RebuildAttributionScope()"))
        self.assertIn("m_pass.frame_instance_count_known() ? "
                      "std::optional<std::size_t>(m_pass.frame_instance_count()) : std::nullopt", scope)

    def test_menu_phase_collection_removes_nested_work_from_parent_buckets(self):
        main = compact(self.main)
        self.assertIn("PhaseFrameScope phase_scope(frame_attribution_enabled);", main)
        for parent in ("menu", "render", "poll", "ui"):
            self.assertIn(f"const double {parent}_phases_before = Attribution::phase_total_ms();", main)
            self.assertIn(f"Attribution::phase_total_ms() - {parent}_phases_before", main)
        self.assertIn("buckets.other(_wallMs, phase_scope.snapshot())", main)
        self.assertIn("buckets.other(frame_ms, phases)", compact(self.collector))
        self.assertIn("count_generation, phases, uploads)",
                      compact(block(self.collector, "bool FrameRecorder::record(")))
        self.assertIn("Phase::WorldCreateCallback", block(self.main, "auto start_world_creation ="))
        self.assertIn("Phase::WorldLoadCallback", block(self.main, "auto start_world_load ="))
        ui = read(CLIENT / "ui/Rml_UIManager.cpp")
        for method, phase in (("Update()", "UiUpdate"), ("Render()", "UiRender"),
                              ("LoadDocument(", "DocumentLoad")):
            self.assertIn(f"Phase::{phase}", block(ui, f"void Rml_UIManager::{method}"))
        for path, marker in (("ui/Rml_Interfaces.cpp", "RmlRenderer::LoadTexture("),
                             ("ui/gl3/RmlUi_Renderer_GL3.cpp", "RenderInterface_GL3::LoadTexture("),
                             ("ui/Rml_Interfaces.cpp", "RmlRenderer::GenerateTexture("),
                             ("ui/gl3/RmlUi_Renderer_GL3.cpp", "RenderInterface_GL3::GenerateTexture(")):
            self.assertIn("Phase::TextureLoad", block(read(CLIENT / path), marker))
        preview = read(CLIENT / "world/WorldgenPreview.cpp")
        self.assertIn("Phase::PreviewAdoption", block(preview, "void WorldgenPreview::swap_pending_into_live("))
        for marker in ("bool WorldgenPreview::render(", "bool WorldgenPreview::render_to_backbuffer("):
            self.assertIn("Phase::MenuScene", block(preview, marker))
        backend = read(CLIENT / "ui/gl3/RmlUi_Renderer_GL3.cpp")
        self.assertIn("Phase::UiFilters", block(backend, "void RenderInterface_GL3::RenderFilters("))
        self.assertIn("Phase::ShaderLoad", block(backend, "static bool CreateShaders("))
        self.assertIn("AttributedGl::UseProgram(glUseProgram,",
                      block(backend, "void RenderInterface_GL3::UseProgram("))
        for path, marker in (("rendering/ImpostorBake.cpp", "AttributedGl::Program CompileBakeProgram("),
                             ("rendering/SkyAtmosphereLut.ipp", "AttributedGl::Program compile_compute("),
                             ("rendering/passes/LuminanceMeterPass.cpp", "AttributedGl::Program create_compute_program("),
                             ("rendering/passes/FoliagePass.cpp", "AttributedGl::Program compile_compute_program(")):
            self.assertIn("Phase::ShaderLoad", block(read(CLIENT / path), marker))
        legacy = read(CLIENT / "ui/Rml_Interfaces.cpp")
        self.assertIn("Phase::ShaderLoad", block(legacy, "RmlRenderer::RmlRenderer()"))
        self.assertIn("AttributedGl::UseProgram(glUseProgram,",
                      block(legacy, "void RmlRenderer::RenderGeometry("))
        self.assertRegex(block(legacy, "void RmlRenderer::RenderGeometry("),
                         r"AttributedGl::Submit\(\s*glDrawElements,")
        shader = read(CLIENT / "rendering/Shader.cpp")
        self.assertIn("Phase::ShaderLoad", block(shader, "AttributedGl::Program Shader::buildProgram("))
        self.assertIn("AttributedGl::UseProgram(glUseProgram,", block(shader, "void Shader::use()"))

    def test_all_program_lifetimes_and_submissions_use_first_use_attribution(self):
        # Enumerate the rendering and both UI backends, including state restores,
        # platform branches, compute kernels, indirect and instanced submissions.
        call = re.compile(r"\bgl(?:CreateProgram|DeleteProgram|UseProgram|"
                          r"Draw(?:Arrays|Elements)\w*|MultiDraw\w*|DispatchCompute\w*)\s*\(")
        count = 0
        for tree in (CLIENT / "rendering", CLIENT / "ui"):
            for path in sorted(tree.rglob("*")):
                if path.suffix not in (".cpp", ".h", ".ipp"):
                    continue
                source = read(path)
                code = re.sub(
                    r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
                    " ", source, flags=re.S)
                self.assertIsNone(call.search(code), str(path.relative_to(ROOT)))
                count += code.count("AttributedGl::Submit(")
        self.assertGreater(count, 40)
        helper = read(CLIENT / "app/ProgramFirstUse.h")
        submit = compact(block(helper, "void submit("))
        self.assertTrue(submit.startswith("if (m_bound->pending) {"))
        self.assertNotIn("PhaseScope", submit)
        self.assertNotIn("lookup(", submit)
        self.assertLess(submit.index("m_bound->pending = false;"),
                        submit.index("if (shader_first_use_collection_enabled)"))
        for method, invocation in (("use_attributed(", "function(program);"),
                                   ("submit_attributed(", "function(std::forward<Args>(args)...);")):
            cold = compact(block(helper, "void " + method))
            self.assertLess(cold.index("++m_attributed_calls;"), cold.index("const PhaseScope"))
            self.assertLess(cold.index("const PhaseScope phase(Phase::ShaderFirstUse);"),
                            cold.index(invocation))
            self.assertIn("LUMINUMBRA_FIRST_USE_COLD void " + method, helper)
            self.assertNotIn("phase.stop", cold)
        use = compact(block(helper, "void use("))
        self.assertIn("if (static_cast<unsigned>(shader_first_use_collection_enabled) & "
                      "static_cast<unsigned>(m_bound->pending))", use)
        self.assertNotIn("PhaseScope", use)
        self.assertNotIn("lookup(", use)
        self.assertEqual(compact(block(helper, "void bind(const Program&")),
                         "m_bound = program.state;")
        self.assertIn("__declspec(noinline)", helper)
        self.assertIn("__attribute__((cold, noinline))", helper)
        gl = read(CLIENT / "rendering/AttributedGl.h")
        self.assertIn("return programs.created(function());", block(gl, "CreateProgram("))
        self.assertIn("programs.deleted(program);", block(gl, "DeleteProgram("))
        self.assertIn("programs.use(function, program);", block(gl, "UseProgram("))
        self.assertIn("programs.submit(function, std::forward<Args>(args)...);",
                      block(gl, "Submit("))
        self.assertNotIn("thread_local", gl)
        self.assertNotIn("PhaseScope", gl)
        self.assertIn("inline constinit bool shader_first_use_collection_enabled = false;",
                      self.header)
        for method in ("PhaseFrameScope::PhaseFrameScope(", "PhaseFrameScope::~PhaseFrameScope()"):
            self.assertIn("shader_first_use_collection_enabled = current_phases() != nullptr;",
                          block(self.collector, method))
        preview = read(ROOT / "tools/static_preview/CMakeLists.txt")
        self.assertIn('"${STATIC_PREVIEW_ROOT}/src/luminumbra_client/app/FrameAttribution.cpp"',
                      preview)

    def test_program_owners_retain_the_first_use_bit_on_every_backend(self):
        for path, members in (
            ("rendering/Shader.h", ("m_id",)),
            ("rendering/passes/FoliagePass.h", ("m_compute_prog", "m_prefix_prog")),
            ("rendering/passes/FroxelPass.h", ("m_inject_program", "m_integrate_program")),
            ("rendering/passes/LuminanceMeterPass.h", ("m_reduce_program",)),
            ("rendering/SkyAtmosphereLut.h", ("m_skyview_compute_prog",
                                           "m_transmittance_compute_prog",
                                           "m_multiscatter_compute_prog")),
            ("ui/Rml_Interfaces.h", ("m_program",)),
        ):
            source = read(CLIENT / path)
            for member in members:
                self.assertIn(f"AttributedGl::Program {member}{{}};", source, path)
        gl3 = read(CLIENT / "ui/gl3/RmlUi_Renderer_GL3.cpp")
        self.assertIn("EnumArray<Luminumbra::Rendering::AttributedGl::Program, ProgramId>", gl3)
        self.assertIn("const AttributedGl::Program& prog",
                      read(CLIENT / "rendering/SkyAtmosphereLut.ipp"))
        # Helpers must not erase the retained bit by converting creation to GLuint.
        for path in CLIENT.rglob("*"):
            if path.suffix in (".cpp", ".h", ".ipp"):
                self.assertNotRegex(read(path),
                                    r"\b(?:GLuint|u32|std::uint32_t)\s+\w+\s*=\s*"
                                    r"(?:Luminumbra::Rendering::)?AttributedGl::CreateProgram\(",
                                    str(path.relative_to(ROOT)))

    def test_nested_phase_scopes_exclude_children_and_foliage(self):
        constructor = compact(block(self.collector, "PhaseScope::PhaseScope("))
        self.assertTrue(constructor.startswith("if (!enabled || !current_phases()) return;"))
        destructor = compact(block(self.collector, "PhaseScope::~PhaseScope()"))
        self.assertTrue(destructor.startswith("if (!m_totals) return;"))
        self.assertIn("m_totals->add(m_phase, elapsed, m_children_ms, foliage - m_child_foliage_ms)",
                      destructor)
        self.assertIn("m_parent->m_children_ms += elapsed;", destructor)
        self.assertIn("m_parent->m_child_foliage_ms += foliage;", destructor)
        rebuild = block(self.foliage, "~RebuildAttributionScope()")
        self.assertIn("Attribution::phase_total_ms() - m_phases_before", rebuild)

    def test_collectors_borrow_mutable_storage_through_thread_local_accessors(self):
        # Check the live accessors and every consumer, so an unused TLS helper
        # cannot hide a process-global pointer or a stale restoration path.
        for signature, declaration, result in (
            ("FoliageFrame*& current_foliage_frame()",
             "thread_local FoliageFrame* frame = nullptr;", "frame"),
            ("PhaseTotals*& current_phases()",
             "thread_local PhaseTotals* totals = nullptr;", "totals"),
            ("const PhaseScope*& current_phase_scope()",
             "thread_local const PhaseScope* scope = nullptr;", "scope"),
        ):
            self.assertIn(signature, self.collector)
            self.assertEqual(compact(block(self.collector, signature)),
                             f"{declaration} return {result};")
        self.assertEqual(len(re.findall(r"\bthread_local\b", self.collector)), 3)
        self.assertNotRegex(self.collector,
                            r"\b(?:current_phases|current_phase_scope)\b(?!\s*\()")
        for owner, storage in (("FoliageFrameScope", "FoliageFrame m_frame{};"),
                               ("PhaseFrameScope", "PhaseTotals m_totals{};")):
            self.assertIn("mutable " + storage, block(self.header, "class " + owner))
        self.assertNotIn("const_cast", self.collector + self.header)
        for marker, fragments in (
            ("PhaseFrameScope::PhaseFrameScope(",
             ("current_phases() = enabled ? &m_totals : nullptr;",)),
            ("PhaseFrameScope::~PhaseFrameScope()",
             ("current_phases() = m_previous;",)),
            ("bool PhaseFrameScope::is_current() const",
             ("return current_phases() == &m_totals;",)),
            ("double phase_total_ms()",
             ("const auto* totals = current_phases();", "return totals ? totals->sum() : 0;")),
            ("PhaseScope::PhaseScope(",
             ("m_totals = current_phases();", "m_parent = current_phase_scope();",
              "current_phase_scope() = this;")),
            ("PhaseScope::~PhaseScope()",
             ("m_parent->m_totals == m_totals", "current_phase_scope() = m_parent;")),
            ("bool PhaseScope::is_current() const",
             ("m_totals == current_phases() && current_phase_scope() == this",)),
        ):
            body = compact(block(self.collector, marker))
            for fragment in fragments:
                self.assertIn(fragment, body)
        self.assertIn(": m_previous(current_phases())", self.collector)

    def test_menu_cache_captures_before_ui_and_bypasses_render_on_reuse(self):
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        self.assertLess(menu.index("copy_scene_image(true)"), menu.index("uiManager->Render()"))
        self.assertLess(menu.index("copy_scene_image(false)"), menu.index("uiManager->Render()"))
        refresh = block(menu, "if (drawable && (refresh || !has_image))")
        self.assertIn("render_to_backbuffer(renderPipeline, render_delta)", refresh)
        self.assertIn("renderPipeline.render_frame(", refresh)
        self.assertIn("schedule.rendered()", refresh)
        reuse = block(menu, "else if (previewState.schedule.valid())")
        self.assertEqual(compact(reuse), "previewState.scene_image.copy_scene_image(false);")
        self.assertIn("document_generation()", menu)
        self.assertIn("worldgenPreview->rebuild_generation()", menu)
        self.assertIn(".renderer_target = renderPipeline.resize_generation()", compact(menu))
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        setter = block(pipeline, "void RenderPipeline::set_render_scale(")
        self.assertIn("resize_graphics_targets(scale, m_shadow_pass->shadow_map().resolution)", setter)
        resize = block(pipeline, "bool RenderPipeline::resize_graphics_targets(")
        self.assertIn("return ApplySceneRenderScale(", resize)
        self.assertLess(resize.index("if (!m_render_registry.replace_targets("),
                        resize.index("++m_resize_generation;"))
        for method in ("void RenderPipeline::on_resize(", "bool RenderPipeline::resize_graphics_targets("):
            self.assertIn("++m_resize_generation;", block(pipeline, method))
        for getter in ("render_scale", "get_exposure_override", "get_cloud_quality",
                       "get_ssao_quality", "volumetric_quality", "taau_enabled",
                       "sky_lut_gpu_enabled", "auto_exposure_metered", "graphics_runtime_config"):
            self.assertIn(f"renderPipeline.{getter}()", menu)
        self.assertLess(menu.index("camera_revision.observe(camera_state())"),
                        menu.index("camera->Yaw = 95.0f"))
        self.assertGreater(menu.index("camera_revision.animated(camera_state())"),
                           menu.index("camera->updateCameraVectors()"))
        self.assertIn("schedule.update(now, key, interactive)", menu)
        self.assertIn("menu_backdrop_yaw += menu_delta", menu)
        self.assertIn("worldgenPreview->tick(menu_delta)", menu)
        self.assertIn("menuPreviewState.schedule = {};", self.main)
        self.assertLess(self.main.index("menuPreviewState.scene_image.release_scene_image();"),
                        self.main.index("glfwDestroyWindow(window);", self.main.index("menuPreviewState.scene_image.release_scene_image();")))

    def test_capture_samples_require_success_and_refusals_replay_without_progress(self):
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        scene = block(menu, "if (scenic || previewActive)")
        refresh = block(scene, "if (drawable && (refresh || !has_image))")
        self.assertIn("bool scene_rendered = false;", menu[:menu.index("if (scenic || previewActive)")])
        self.assertEqual(menu.count("scene_rendered = true;"), 1)
        self.assertEqual(menu.count("schedule.rendered();"), 1)
        self.assertIn("bool rendered = false;", refresh)
        preview = compact(block(refresh, "if (previewActive)"))
        self.assertEqual(preview,
                         "rendered = worldgenPreview->render_to_backbuffer(renderPipeline, render_delta);")
        scenic = compact(block(refresh, "else"))
        self.assertTrue(scenic.endswith("rendered = true;"))
        self.assertLess(scenic.index("renderPipeline.render_frame("), scenic.index("rendered = true;"))
        success = compact(block(refresh, "if (rendered)"))
        self.assertTrue(success.startswith("scene_rendered = true;"))
        self.assertIn("previewState.schedule.rendered();", success)
        refused = compact(block(refresh, "else if (has_image && previewState.schedule.valid())"))
        self.assertEqual(refused, "previewState.scene_image.copy_scene_image(false);")
        self.assertLess(menu.index("uiManager->Render();"), menu.index("bool ready_to_capture = false;"))

    def test_capture_settling_evaluates_live_throttle_and_admission_predicates(self):
        # Evaluate the shipping scheduler and counter guards, not a separately
        # hardcoded cadence. Binding checks above keep render success outside replay.
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        regular = block(menu, "else if (app.capture.ui_screenshot_settle < 30)")
        regular = re.sub(r"//[^\n]*", "", regular)
        admission = re.fullmatch(r"\s*if \((.*?)\)\s*"
                                r"\+\+app.capture.ui_screenshot_settle;\s*", regular, re.S)
        self.assertIsNotNone(admission)
        predicate = admission.group(1)
        for scenic, preview, rendered in ((False, False, False), (True, False, False),
                                         (False, True, False), (True, False, True),
                                         (False, True, True)):
            self.assertEqual(cpp_expression(predicate, scene_rendered=rendered,
                                            scenic=scenic, previewActive=preview),
                             rendered or not (scenic or preview))
        schedule = read(CLIENT / "app/MenuSceneSchedule.h")
        update = block(schedule, "bool update(")
        render_delta = re.sub(r"//[^\n]*", "", block(schedule, "float render_delta_seconds() const"))
        self.assertEqual(compact(render_delta),
                         "return std::min(std::chrono::duration<float>(m_now - m_last_render).count(), 1.0f / 20.0f);")
        self.assertIn("m_last_render = m_now;", block(schedule, "void rendered()"))
        self.assertIn("m_now = now;", update)
        slot_expression = re.search(r"const auto slot = (.*?);", update, re.S).group(1)
        refresh_expression = re.search(r"return (.*?);", update, re.S).group(1)
        second = int(re.search(r"kSecond = ([\d']+);", schedule).group(1).replace("'", ""))
        last_slot, valid, samples, replayed = -1, False, 0, 0
        for tick in range(1001):  # 1000 Hz UI over exactly one elapsed second
            slot = cpp_expression(slot_expression, elapsed=tick * 1_000_000, kSecond=second)
            refresh = cpp_expression(refresh_expression, **{
                "key.width": 1920, "key.height": 1080, "m_valid": valid,
                "due": slot != last_slot, "interactive": False})
            last_slot = slot
            if refresh:
                valid = True
            else:
                replayed += 1
            samples += bool(cpp_expression(predicate, scene_rendered=refresh,
                                           scenic=True, previewActive=False))
            if tick == 30:
                self.assertEqual(samples, 1)  # 31 UI frames cannot settle a scene
            if tick == 966:
                self.assertEqual(samples, 29)
        self.assertEqual((samples, replayed), (31, 970))
        for width, height, interactive, expected in ((0, 1080, True, False),
                                                      (1920, 0, True, False),
                                                      (1920, 1080, True, True),
                                                      (1920, 1080, False, False)):
            self.assertEqual(cpp_expression(refresh_expression, **{
                "key.width": width, "key.height": height, "m_valid": True,
                "due": False, "interactive": interactive}), expected)

    def test_render_delta_matches_shipping_ceiling_and_preserves_elapsed_consumers(self):
        schedule = read(CLIENT / "app/MenuSceneSchedule.h")
        body = re.sub(r"//[^\n]*", "", block(schedule, "float render_delta_seconds() const"))
        bounded = re.fullmatch(
            r"\s*return std::min\(std::chrono::duration<float>\(m_now - m_last_render\)"
            r"\.count\(\), ([\d.]+)f / ([\d.]+)f\);\s*", body)
        self.assertIsNotNone(bounded)
        numerator, denominator = map(float, bounded.groups())
        ceiling = numerator / denominator
        shipping = re.search(r"deltaTime = std::min\(deltaTime, ([\d.]+)f / ([\d.]+)f\);", self.main)
        self.assertIsNotNone(shipping)
        self.assertEqual(ceiling, float(shipping[1]) / float(shipping[2]))
        self.assertEqual(ceiling, 0.05)
        # Evaluate the scalar operation extracted above, without native execution.
        for elapsed, expected in ((0, 0), (0.001, 0.001), (0.033, 0.033),
                                  (0.049, 0.049), (0.05, 0.05), (0.051, 0.05),
                                  (0.730, 0.05), (60, 0.05), (99.9, 0.05),
                                  (-0.001, -0.001)):
            with self.subTest(elapsed=elapsed):
                self.assertEqual(min(elapsed, ceiling), expected)
        self.assertEqual(compact(block(schedule, "double elapsed_seconds() const")),
                         "return std::chrono::duration<double>(m_now - m_epoch).count();")
        self.assertEqual(compact(block(schedule, "void invalidate()")), "m_valid = false;")
        self.assertEqual(compact(block(schedule, "void rendered()")),
                         "m_valid = true; m_last_render = m_now;")
        update = block(schedule, "bool update(")
        self.assertEqual(compact(block(update, "if (!m_started)")),
                         "m_started = true; m_epoch = now; m_last_render = now;")
        self.assertEqual(update.count("m_last_render ="), 1)
        self.assertEqual(update.count("m_epoch ="), 1)
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        self.assertIn("std::chrono::duration<float>(menu_now - previewState.last_menu_frame).count()", menu)
        self.assertIn("app.menu.menu_backdrop_yaw += menu_delta;", menu)
        self.assertIn("worldgenPreview->tick(menu_delta);", menu)
        self.assertIn("std::chrono::steady_clock::now()", menu)
        refresh = block(menu, "if (drawable && (refresh || !has_image))")
        self.assertIn("const float render_delta = previewState.schedule.render_delta_seconds();", refresh)
        self.assertIn("worldgenPreview->render_to_backbuffer(renderPipeline, render_delta)", refresh)
        self.assertIn("*camera, render_delta,", compact(refresh))
        preview = block(read(CLIENT / "world/WorldgenPreview.cpp"), "bool WorldgenPreview::render_to_backbuffer(")
        self.assertIn("pipeline.render_frame(m_registry, *m_world, cam, dt,", preview)
        pipeline = read(CLIENT / "rendering/RenderPipeline.cpp")
        prepare = block(pipeline, "void RenderPipeline::prepare_frame(")
        self.assertIn("m_particle_pass->update(deltaTime);", prepare)
        self.assertIn("m_frame_prepared.delta_time = deltaTime;", prepare)
        self.assertIn("RenderMenuScreens(g_app, window, deltaTime,", compact(self.main))
        self.assertLess(shipping.start(), self.main.index("RenderMenuScreens(g_app,"))
        thumbnails = block(menu, "if (app.capture.ui_thumbs > 0")
        self.assertIn("*camera, deltaTime,", compact(thumbnails))

    def test_preview_capture_requires_ready_successes_and_keeps_the_timeout(self):
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        preview = block(menu, "if (preview_live_screen)")
        readiness = preview[preview.index("const bool world_ready"):]
        reset = re.search(r"if \((.*?)\)\s*app.capture.ui_preview_settle_after_ready = 0;",
                          readiness, re.S).group(1)
        increment = re.search(r"else if \((.*?)\)\s*\+\+app.capture.ui_preview_settle_after_ready;",
                              readiness, re.S).group(1)
        settled = re.search(r"const bool settled = (.*?);", readiness, re.S).group(1)
        timeout = re.search(r"const bool timed_out = (.*?);", readiness, re.S).group(1)
        self.assertRegex(compact(readiness), r"kPreviewTimeoutSeconds = 45\.0;")
        self.assertRegex(compact(readiness), r"kPreviewSettleAfterReady = 40;")
        self.assertIn("ready_to_capture = settled || timed_out;", readiness)
        self.assertIn("worldgenPreview && worldgenPreview->world_ready()", preview)
        armed = block(preview, "if (!s_preview_wait_armed)")
        self.assertIn("app.capture.ui_preview_settle_after_ready = 0;", armed)
        self.assertIn("s_preview_wait_start = std::chrono::steady_clock::now();", armed)
        self.assertEqual(menu.count("s_preview_wait_start = std::chrono::steady_clock::now();"), 1)
        count = 0
        # Pending/missing world, successful renders, cache replays, refused renders,
        # readiness loss, inactive preview, then a fresh uninterrupted warmup.
        for ready, active, rendered, expected in (
                [(False, True, False, 0)] + [(True, True, True, n) for n in range(1, 40)] +
                [(True, True, False, 39), (False, True, False, 0),
                 (True, True, True, 1), (True, False, False, 0)] +
                [(True, True, True, n) for n in range(1, 41)]):
            values = dict(world_ready=ready, previewActive=active, scene_rendered=rendered)
            if cpp_expression(reset, **values):
                count = 0
            elif cpp_expression(increment, **values):
                count += 1
            self.assertEqual(count, expected)
            self.assertEqual(cpp_expression(settled, **{
                "world_ready": ready, "app.capture.ui_preview_settle_after_ready": count,
                "kPreviewSettleAfterReady": 40}), ready and count >= 40)
        for waited, expected in ((0, False), (44.999, False), (45, True), (46, True)):
            self.assertEqual(cpp_expression(timeout, waited_s=waited,
                                            kPreviewTimeoutSeconds=45.0), expected)

    def test_capture_invalidation_batch_advancement_and_thumbnail_siblings(self):
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        transition = block(menu, "if (!previewState.capture_key || key != *previewState.capture_key)")
        for reset in ("app.capture.ui_screenshot_settle = 0;",
                      "app.capture.ui_preview_settle_after_ready = 0;",
                      "previewState.capture_key = key;"):
            self.assertIn(reset, transition)
        self.assertLess(menu.index("previewState.capture_key = key;"),
                        menu.index("schedule.update(now, key, interactive)"))
        self.assertIn("std::optional<MenuSceneKey> capture_key;", read(CLIENT / "app/MenuScreens.h"))
        without_scene = block(menu[menu.index("} else if (previewState.schedule.valid())"):], "else {")
        reset = block(without_scene, "if (previewState.capture_key)")
        for fragment in ("app.capture.ui_screenshot_settle = 0;",
                         "app.capture.ui_preview_settle_after_ready = 0;",
                         "previewState.capture_key.reset();"):
            self.assertIn(fragment, reset)
        # The inner ready log has no capture writes; use the final ready block.
        capture = block(menu[menu.index("} else if (app.capture.ui_screenshot_settle < 30)"):],
                        "if (ready_to_capture)")
        self.assertIn("s_preview_wait_armed = false;", capture)
        self.assertLess(capture.index("WritePixelBufferPpm("), capture.index("++app.capture.ui_screen_index;"))
        for fragment in ("app.capture.ui_screenshot_settle = 0;",
                         "app.capture.ui_preview_settle_after_ready = 0;",
                         "previewState.capture_key.reset();", "RequestLoadDocument(",
                         "glfwSetWindowShouldClose(window, GLFW_TRUE);"):
            self.assertIn(fragment, capture)
        self.assertIn("app.capture.ui_screen_index < app.capture.ui_screens.size()", menu)
        thumbs = block(menu, "if (app.capture.ui_thumbs > 0")
        self.assertNotIn("schedule.update", thumbs)
        self.assertLess(thumbs.index("renderPipeline.render_frame("), thumbs.index("++app.capture.ui_thumbs_settle;"))
        self.assertIn("app.capture.ui_thumbs_settle < 28", thumbs)
        self.assertIn("app.capture.ui_thumbs_settle = 0;", thumbs)
        self.assertIn("++app.capture.ui_thumbs_index;", thumbs)

    def test_capture_gate_expires_refused_ordinary_and_live_preview(self):
        for content_ready in (False, True):
            gate = CaptureGateEvaluation()
            self.assertTrue(gate.arm())
            self.assertEqual(gate.decide(44.999, content_ready=content_ready), "Waiting")
            self.assertFalse(gate.arm(44.999))
            self.assertEqual(gate.decide(45, content_ready=content_ready), "TimedOut")
            self.assertEqual(gate.decide(46, settled=True), "TimedOut")
        gate = CaptureGateEvaluation()
        self.assertEqual(gate.decide(45, settled=True), "Waiting")
        gate.arm()
        self.assertEqual(gate.decide(44.999, settled=True), "Ready")
        self.assertEqual(gate.decide(45, settled=True), "TimedOut")

    def test_real_capture_policy_refused_first_candidate_fails_both_modes(self):
        for live in (False, True):
            capture = MenuCaptureEvaluation(live=live)
            for tick in range(45):
                self.assertEqual(capture.frame(tick), "Waiting")
                self.assertEqual(capture.count, 0)
            self.assertEqual(capture.frame(45), "TimedOut")

    def test_real_capture_policy_wait_replay_refusal_and_actual_renders(self):
        for live, needed in ((False, 30), (True, 40)):
            capture = MenuCaptureEvaluation(live=live)
            self.assertEqual(capture.frame(0), "Waiting")
            for tick in range(1, 40):
                self.assertEqual(capture.frame(tick / 100, ready=True), "Waiting")
                self.assertEqual(capture.count, 0)
            # A rejected new candidate may leave an older world drawable.
            self.assertEqual(capture.frame(1, rendered=True, ready=True, failed=True), "Waiting")
            self.assertEqual(capture.count, 0)
            for sample in range(1, needed + 1):
                result = capture.frame(2 + sample / 100, rendered=True, ready=True)
                self.assertEqual(capture.count, sample)
                self.assertEqual(result, "Ready" if live and sample == needed else "Waiting")
            self.assertEqual(capture.frame(3, ready=True), "Ready")

    def test_real_capture_policy_ui_layout_and_readiness_loss(self):
        capture = MenuCaptureEvaluation()
        for tick in range(30):
            self.assertEqual(capture.frame(tick / 100, preview=False), "Waiting")
        self.assertEqual(capture.frame(0.3, preview=False), "Ready")
        for missing in (dict(drawable=False), dict(document=False)):
            self.assertEqual(capture.frame(1, preview=False, **missing), "Waiting")
            self.assertEqual(capture.count, 0)
        self.assertEqual(capture.frame(45, preview=False), "TimedOut")

    def test_refused_source_cannot_be_cleared_by_an_older_successful_build(self):
        for live in (False, True):
            capture = MenuCaptureEvaluation(live=live)
            self.assertEqual(capture.frame(0, failed=True, source_failed=True), "Waiting")
            # An earlier/default candidate can finish after this request was refused.
            for sample in range(1, 100):
                self.assertEqual(capture.frame(sample / 10, rendered=True, ready=True,
                                               source_failed=True), "Waiting")
                self.assertEqual(capture.count, 0)
            self.assertEqual(capture.frame(45, rendered=True, ready=True,
                                           source_failed=True), "TimedOut")
            self.assertEqual(capture.frame(46, rendered=True, ready=True, index=1), "Waiting")
            self.assertEqual(capture.count, 1)

        source = read(CLIENT / "world/WorldgenPreview.cpp")
        admission = block(source, "bool WorldgenPreview::set_candidate(")
        refused = block(admission, "if (!result.ok)")
        self.assertIn("return false;", refused)
        self.assertNotIn("set_params(", refused)
        self.assertLess(admission.index("return false;"), admission.index("m_data_root = data_root;"))
        self.assertLess(admission.index("set_params(result.params, seed);"),
                        admission.index("return true;"))
        candidate = block(read(CLIENT / "app/MenuScreens.cpp"), "if (sig != previewState.lastSig)")
        self.assertIn("previewState.candidate_source_failed = "
                      "!worldgenPreview->set_candidate(resolved, data_root, seedVal);", compact(candidate))

    def test_older_world_cannot_settle_a_pending_or_refused_replacement(self):
        readiness = block(read(CLIENT / "world/WorldgenPreview.h"), "bool candidate_ready() const")
        predicate = re.fullmatch(r"\s*return (.*?);\s*", readiness, re.S).group(1)
        for world, dirty, inflight, failed in ((False, False, False, False),
                                             (True, True, False, False),
                                             (True, False, True, False),
                                             (True, False, False, True),
                                             (True, False, False, False)):
            self.assertEqual(cpp_expression(predicate, **{
                "world_ready()": world, "m_pending_dirty": dirty,
                "m_build_inflight.load()": inflight, "m_last_build_failed": failed}),
                world and not (dirty or inflight or failed))
        for live, required in ((False, 30), (True, 40)):
            capture = MenuCaptureEvaluation(live=live)
            for sample in range(100):
                self.assertEqual(capture.frame(sample / 10, rendered=True, ready=True,
                                               candidate_ready=False), "Waiting")
                self.assertEqual(capture.count, 0)
            for sample in range(required):
                capture.frame(11 + sample / 100, rendered=True, ready=True, candidate_ready=True)
            self.assertEqual(capture.count, required)
            self.assertEqual(capture.frame(12, ready=True, candidate_ready=True), "Ready")
            self.assertEqual(capture.frame(13, rendered=True, ready=True, candidate_ready=False), "Waiting")
            self.assertEqual(capture.count, 0)
            self.assertEqual(capture.frame(45, ready=True, candidate_ready=False), "TimedOut")

    def test_capture_gate_rearms_for_screen_advance_and_mode_changes_only(self):
        gate = CaptureGateEvaluation()
        gate.arm()
        self.assertEqual(gate.decide(44, settled=True), "Ready")
        self.assertTrue(gate.arm(44, index=1))
        self.assertEqual(gate.decide(45), "Waiting")
        for tick in range(45, 89):
            self.assertFalse(gate.arm(tick, index=1))
        self.assertEqual(gate.decide(89), "TimedOut")
        self.assertTrue(gate.arm(89, index=1, thumbnail=True))
        self.assertEqual(gate.decide(90, settled=True), "Ready")
        self.assertFalse(gate.arm(133, index=1, thumbnail=True))
        self.assertEqual(gate.decide(134), "TimedOut")
        gate.call("void reset()")
        self.assertEqual(gate.decide(150, settled=True), "Waiting")
        self.assertTrue(gate.arm(150, index=1, thumbnail=True))

    def test_capture_gate_uses_real_settling_admission_for_wait_replay_and_ui(self):
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        regular = re.sub(r"//[^\n]*", "", block(menu, "else if (app.capture.ui_screenshot_settle < 30)"))
        predicate = re.fullmatch(r"\s*if \((.*?)\)\s*"
                                 r"\+\+app.capture.ui_screenshot_settle;\s*", regular, re.S).group(1)
        for scenic, preview in ((True, False), (False, True), (False, False)):
            gate = CaptureGateEvaluation()
            gate.arm()
            count = 0
            for tick in range(120):
                rendered = tick >= 60 and tick % 2 == 0
                settled = count >= 30  # The shipping ordinary branch captures on the next frame.
                if not settled and cpp_expression(predicate, scene_rendered=rendered,
                                                   scenic=scenic, previewActive=preview):
                    count += 1
                expected = "Ready" if (tick >= 30 if not (scenic or preview) else tick == 119) else "Waiting"
                self.assertEqual(gate.decide(tick / 60, settled=settled), expected)
            self.assertEqual(count, 30)

    def test_capture_target_or_content_loss_cannot_publish_or_extend_deadline(self):
        for drawable, content in ((False, True), (True, False), (False, False)):
            gate = CaptureGateEvaluation()
            gate.arm()
            for tick in range(45):
                self.assertEqual(gate.decide(tick, settled=True, drawable=drawable,
                                            content_ready=content), "Waiting")
                self.assertFalse(gate.arm(tick))
            self.assertEqual(gate.decide(45, settled=True, drawable=drawable,
                                        content_ready=content), "TimedOut")

    def test_capture_deadline_is_armed_before_every_render_and_failure_closes(self):
        source = read(CLIENT / "app/MenuScreens.cpp")
        menu = block(source, "void RenderMenuScreens(")
        setup = block(source, "bool PrepareMenuCapture(")
        capturing = block(setup, "if (capturing)")
        self.assertLess(capturing.index("capture_gate.arm("), capturing.index("capture_gate.decide("))
        self.assertLess(menu.index("if (!PrepareMenuCapture("), menu.index("renderPipeline.render_frame("))
        self.assertRegex(compact(menu), r"if \(!PrepareMenuCapture\([^;]*\)\) return;")
        self.assertIn("const bool capturing = thumbnails_pending || screenshot_pending;", compact(setup))
        self.assertIn("!thumbnails_pending && !app.capture.ui_screens.empty()", menu)
        failure = block(source, "void FailMenuCapture(")
        self.assertIn("previewState.capture_failed = true;", failure)
        self.assertIn("glfwSetWindowShouldClose(window, GLFW_TRUE);", failure)
        self.assertRegex(compact(self.main), r"if \(menuPreviewState.capture_failed && exit_code == 0\) exit_code = 1;")
        self.assertNotIn("static bool s_preview_wait_armed", source)
        self.assertIn("previewState.preview_wait_armed = false;", capturing)

    def test_capture_refusal_missing_document_and_zero_size_reset_settling(self):
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        content = re.search(r"const bool content_ready\s*=\s*(.*?);", read(CLIENT / "app/MenuScreens.cpp"), re.S).group(1)
        for fragment in ("previewState.capture_document_ready", "previewActive && worldgenPreview",
                         "worldgenPreview->world_ready()", "!worldgenPreview->last_build_failed()",
                         "worldgenPreview->candidate_ready()", "!previewState.candidate_source_failed"):
            self.assertIn(fragment, content)
        ready = {"previewState.capture_document_ready": True,
                 "app.menu.menu_backdrop_active": False, "scenic": False,
                 "previewActive": True, "expects_preview": True, "worldgenPreview": True,
                 "worldgenPreview->world_ready()": True,
                 "worldgenPreview->candidate_ready()": True,
                 "worldgenPreview->last_build_failed()": False,
                 "previewState.candidate_source_failed": False}
        self.assertTrue(cpp_expression(content, **ready))
        for name in ("previewState.capture_document_ready", "previewActive", "worldgenPreview",
                     "worldgenPreview->world_ready()", "worldgenPreview->last_build_failed()",
                     "worldgenPreview->candidate_ready()", "previewState.candidate_source_failed"):
            refused = dict(ready)
            refused[name] = not refused[name]
            self.assertFalse(cpp_expression(content, **refused))
        ui_only = dict(ready, expects_preview=False, previewActive=False, worldgenPreview=False)
        self.assertTrue(cpp_expression(content, **ui_only))
        ui_only["app.menu.menu_backdrop_active"] = True
        self.assertFalse(cpp_expression(content, **ui_only))
        reset = block(menu, "if (!content_ready || !drawable)")
        for fragment in ("app.capture.ui_screenshot_settle = 0;",
                         "app.capture.ui_preview_settle_after_ready = 0;", "ready_to_capture = false;"):
            self.assertIn(fragment, reset)
        candidate = block(menu, "if (sig != previewState.lastSig)")
        self.assertLess(candidate.index("candidate_source_failed = true;"), candidate.index("std::ifstream in"))
        self.assertIn("previewState.candidate_source_failed = "
                      "!worldgenPreview->set_candidate(resolved, data_root, seedVal);", compact(candidate))
        setup = block(read(CLIENT / "app/MenuScreens.cpp"), "bool PrepareMenuCapture(")
        self.assertIn("requested && requested->IsVisible()", setup)
        self.assertIn("context->GetDocument(documents - 1)", setup)
        self.assertIn("requested->GetSourceURL().ends_with(", setup)
        self.assertIn("if (armed || capture_width != previewState.capture_width", setup)
        self.assertEqual(setup.count("capture_gate.arm("), 1)
        drawable = re.search(r"const bool drawable = (.*?);", menu[menu.index("const bool expects_preview"):], re.S).group(1)
        for width, height in ((0, 0), (0, 720), (1280, 0), (-1, 720), (1280, -1), (1280, 720)):
            self.assertEqual(cpp_expression(drawable, capture_width=width, capture_height=height),
                             width > 0 and height > 0)

    def test_capture_final_decisions_and_output_errors_precede_advancement(self):
        menu = block(read(CLIENT / "app/MenuScreens.cpp"), "void RenderMenuScreens(")
        setup = block(read(CLIENT / "app/MenuScreens.cpp"), "bool PrepareMenuCapture(")
        self.assertEqual((menu + setup).count("MenuCaptureGate::Result::TimedOut"), 3)
        capture = menu[menu.index("const bool expects_preview"):]
        self.assertIn("ready_to_capture = decision == MenuCaptureGate::Result::Ready;", capture)
        self.assertIn("capture_gate.decide( std::chrono::steady_clock::now(), "
                      "ready_to_capture, drawable, content_ready)", compact(capture))
        self.assertLess(capture.index("MenuCaptureGate::Result::TimedOut"), capture.index("if (ready_to_capture)"))
        for body, advance in ((capture, "++app.capture.ui_screen_index;"),
                              (block(menu, "if (app.capture.ui_thumbs > 0"), "++app.capture.ui_thumbs_index;")):
            refusal = block(body, "if (!WritePixelBufferPpm(")
            self.assertIn("fail_capture(", refusal)
            self.assertIn("return;", refusal)
            self.assertLess(body.index("if (!WritePixelBufferPpm("), body.index(advance))
        output = block(read(CLIENT / "app/MenuScreens.cpp"), "bool WritePixelBufferPpm(")
        self.assertLess(output.index("if (!rgba_bytes)"), output.index("std::vector<unsigned char>"))
        self.assertIn("(*rgba_bytes / 4) * 3", output)
        self.assertIn("return WritePixelBufferPpm(shot, width, height, pixels);", output)
        self.assertIn("return false;", block(output, "catch (const std::exception& error)"))

    def test_capture_writer_reports_write_flush_and_close_failures(self):
        writer = block(read(CLIENT / "rendering/PixelIo.cpp"), "bool WritePixelBufferPpm(")
        tail = writer[writer.index("output.write("):]
        helper = read(CLIENT / "rendering/PixelIo.h")
        flush = block(helper, "bool FlushCaptureStream(")
        self.assertLess(flush.index("output.flush();"), flush.index("return static_cast<bool>(output);"))
        close = block(helper, "bool CloseCaptureFile(")
        self.assertLess(close.index("FlushCaptureStream(output)"), close.index("output.close();"))
        self.assertIn("return flushed && static_cast<bool>(output);", close)
        failure = block(tail, "if (!CloseCaptureFile(output))")
        self.assertIn("return false;", failure)
        self.assertIn("Failed to finish screenshot artifact", failure)
        self.assertLess(tail.index("if (!CloseCaptureFile(output))"), tail.index("return true;"))

    def test_thumbnail_keys_and_readback_refusals_preserve_batch_progress(self):
        source = read(CLIENT / "app/MenuScreens.cpp")
        pending = re.fullmatch(r"\s*return (.*?);\s*",
                               block(source, "bool MenuThumbnailsPending("), re.S).group(1)
        for requested, index in ((0, 0), (-1, 0), (1, 0), (1, 1), (1, 2), (2, 1)):
            self.assertEqual(cpp_expression(pending, **{
                "app.capture.ui_thumbs": requested, "app.capture.ui_thumbs_index": index}),
                requested > 0 and index < requested)
        menu = block(source, "void RenderMenuScreens(")
        thumbs = block(menu, "if (app.capture.ui_thumbs > 0")
        scene = block(menu, "if (scenic || previewActive)")
        self.assertEqual(compact(block(thumbs, ".settings =")),
                         compact(block(scene, ".settings =")))
        for fragment in (".width = capture_width", ".height = capture_height",
                         ".document = previewState.capture_document",
                         ".renderer_target = renderPipeline.resize_generation()",
                         ".camera = previewState.camera_revision.observe(MenuCameraState(*camera))",
                         ".wireframe = app.overlay.wireframe_mode"):
            self.assertIn(fragment, thumbs)
        self.assertLess(thumbs.index("camera->updateCameraVectors();"), thumbs.index("thumbnail_key{"))
        changed = block(thumbs, "if (previewState.capture_key != thumbnail_key)")
        self.assertIn("app.capture.ui_thumbs_settle = 0;", changed)
        self.assertIn("previewState.capture_key = thumbnail_key;", changed)
        self.assertNotIn("capture_gate.arm", changed)
        zero = block(thumbs, "if (capture_width <= 0 || capture_height <= 0)")
        self.assertIn("app.capture.ui_thumbs_settle = 0;", zero)
        self.assertIn("return;", zero)
        self.assertIn("app.capture.ui_thumbs_index >= app.capture.ui_thumbs && "
                      "app.capture.ui_screen_index >= app.capture.ui_screens.size()", compact(thumbs))
        readback = block(source, "bool WritePixelBufferPpm(const std::filesystem::path& shot, GLFWwindow* window)")
        self.assertRegex(compact(readback), r"if \(!WritePixelBufferPpm\(shot, width, height\)\) return false;")
        self.assertLess(readback.index("return false;"), readback.index("UI screenshot written"))

    def test_blur_reuses_geometry_and_kernels_without_changing_the_passes(self):
        backend = read(CLIENT / "ui/gl3/RmlUi_Renderer_GL3.cpp")
        quad = block(backend, "void RenderInterface_GL3::DrawFullscreenQuad(Rml::Vector2f")
        identity = block(quad, "if (uv_offset == Rml::Vector2f() && uv_scaling == Rml::Vector2f(1.f))")
        self.assertEqual(compact(identity), "DrawFullscreenQuad(); return;")
        hit = block(quad, "if (const auto* cached = filter_quad_cache.find(key))")
        self.assertNotIn("CompileGeometry", hit)
        self.assertNotIn("ReleaseGeometry", hit)
        self.assertIn("return;", hit)
        self.assertIn("filter_quad_cache.store(key, geometry)", quad)
        kernel = block(backend, "static void SetBlurWeights(")
        hit = block(kernel, "if (const auto* cached = kernels.find(key))")
        self.assertIn("glUniform1fv", hit)
        self.assertNotIn("Exp(", hit)
        self.assertIn("kernels.store(key, weights)", kernel)

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

    def test_metadata_replacement_does_not_upload_a_redundant_command(self):
        gpu = block(self.foliage, "bool FoliagePass::rebuild_instances_gpu(")
        self.assertIn("const bool initialize_command = !m_metadata_valid;", gpu)
        reset = compact(block(gpu, "if (initialize_command || !metadata_changed)"))
        for operation in ("glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_count_ssbo);",
                          "glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(draw_command), draw_command);",
                          "m_rebuild_attribution.uploaded_bytes += sizeof(draw_command);",
                          "++m_rebuild_attribution.uploads;"):
            self.assertIn(operation, reset)
        self.assertEqual(gpu.count("sizeof(draw_command), draw_command)"), 1)
        # Count never consumes the old command. Prefix writes every word, then
        # the storage barrier makes those writes visible before emit/draw.
        scatter = read(ROOT / "res/shaders/grass_scatter.comp")
        for name in ("append_count", "draw_vertex_count", "draw_instance_count",
                     "draw_first_vertex", "draw_base_instance"):
            self.assertEqual(len(re.findall(r"\b" + name + r"\b", scatter)), 2 if name == "append_count" else 1)
        prefix = compact(block(read(ROOT / "res/shaders/grass_compact_prefix.comp"), "void main()"))
        for assignment in ("accepted_count = total;", "vertex_count = 12u;",
                           "instance_count = min(total, u_max_instances);",
                           "first_vertex = 0u;", "base_instance = 0u;"):
            self.assertIn(assignment, prefix)
        dispatch = compact(block(self.foliage, "void FoliagePass::dispatch_scatter()"))
        self.assertIn("Luminumbra::Rendering::AttributedGl::Submit(glDispatchCompute, 1, 1, 1); "
                      "glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT); "
                      "Luminumbra::Rendering::AttributedGl::UseProgram(glUseProgram, m_compute_prog); "
                      "glUniform1i(m_scatter_uniforms.emit, GL_TRUE);", dispatch)

    def test_csv_schema_keeps_nullable_counts_before_dirty_columns(self):
        header = block(self.collector, "void write_csv_header(")
        static_header, phase_header = header.split("    for (std::size_t index", 1)
        encoded = "".join(json.loads(literal) for literal in re.findall(r'"(?:\\.|[^"\\])*"', static_header))
        self.assertEqual(compact("for (std::size_t index" + phase_header),
                         "for (std::size_t index = 0; index < kPhaseCount; ++index) "
                         "out << ',' << phase_name(static_cast<Phase>(index)) << \"_ms\"; out << '\\n';")
        self.assertEqual(encoded.rstrip("\n").split(","), [
            "frame", "frame_ms", "sim_ms", "stream_ms", "render_ms", "present_ms",
            "foliage_ms", "scatter_ms", "rebake_ms", "poll_ms", "menu_ms", "ui_ms", "other_ms",
            "signature_ms", "sort_ms", "surface_ms", "repack_ms", "hash_ms", "upload_ms",
            "palette_scratch_ms", "dispatch_ms", "cpu_fallback_ms", "rebuild_calls", "reason_mask",
            "admitted_columns", "surface_misses", "deferred_columns", "copied_bytes", "hashed_bytes",
            "uploaded_bytes", "scratch_bytes", "backlog", "path", "fallback", "instance_capacity",
            "generated_count", "count_generation", "dirty_columns",
            "terrain_work_ms", "terrain_overshoot_ms", "terrain_attempts", "terrain_adopted",
            "terrain_work_bytes", "terrain_byte_overshoot", "terrain_pending", "water_work_ms",
            "water_overshoot_ms", "water_attempts", "water_adopted", "water_work_bytes",
            "water_byte_overshoot", "water_pending",
        ])
        row = compact(block(self.collector, "bool write_csv_row("))
        self.assertIn("if (count_known) out << generated_count; out << ','; "
                      "if (count_known) out << count_generation; "
                      "out << ',' << foliage.dirty_columns;", row)
        self.assertLess(row.index("foliage.dirty_columns"), row.index("append_uploads("))

    def test_camera_transition_fixture_starts_with_an_attributed_baseline(self):
        native = read(ROOT / "test/rendering/foliage_rebuild_test.cpp")
        transition = compact(block(native, "TEST_F(FoliageRebuildTest, "
                                   "CameraReorderUploadsMetadataOnceAndPrefixReplacesEveryCommandWord)"))
        baseline, movement = transition.split("for (const glm::vec3 camera :", 1)
        self.assertIn("const auto initial = RebuildAttributed(reused.pass, chunks, 1);", baseline)
        self.assertIn("EXPECT_EQ(initial.reasons, Attribution::Initial);", baseline)
        self.assertNotIn("Rebuild(reused.pass, chunks, 1);", baseline)
        self.assertIn("glm::vec3{kCamera.x + 32, kCamera.y, kCamera.z}, kCamera", movement)
        self.assertIn("RebuildAttributed(reused.pass, chunks, ++frame, camera)", movement)
        self.assertIn("EXPECT_EQ(work.reasons, Attribution::CameraCell);", movement)
        self.assertIn("for (const bool gpu : {false, true})", movement)
        self.assertIn("Rebuild(fresh.pass, chunks, frame, camera);", movement)

    def test_legacy_hash_oracle_uses_the_callers_camera_and_preserves_the_default(self):
        native = read(ROOT / "test/rendering/foliage_rebuild_test.cpp")
        self.assertRegex(compact(native), r"LegacyUploadedHash\(const FoliagePass& pass, "
                         r"std::size_t count, const glm::vec3& camera = kCamera\)")
        oracle = compact(block(native, "static std::uint64_t LegacyUploadedHash("))
        self.assertIn("{camera.x, camera.z, pass.density_scale(), pass.fade_end_m(), total_weight}",
                      oracle)
        self.assertNotIn("kCamera", oracle)
        transition = compact(block(native, "TEST_F(FoliageRebuildTest, "
                                   "CameraReorderUploadsMetadataOnceAndPrefixReplacesEveryCommandWord)"))
        self.assertIn("EXPECT_EQ(hash, LegacyUploadedHash(reused.pass, chunks.size(), camera));",
                      transition)

    def test_csv_nullable_counts_precede_both_complete_upload_lanes(self):
        writer = compact(block(self.collector, "bool write_csv_row("))
        self.assertIn("if (count_known) out << generated_count; out << ','; "
                      "if (count_known) out << count_generation;", writer)
        self.assertLess(writer.index("if (count_known)"), writer.index("append_uploads("))
        self.assertIn("append_uploads(uploads.terrain, uploads.terrain_pending); "
                      "append_uploads(uploads.water, uploads.water_pending); "
                      "for (const double value : phases.milliseconds()) out << ',' << value; "
                      "out << '\\n';",
                      writer)
        append = compact(block(writer, "const auto append_uploads ="))
        self.assertEqual(append, "out << ',' << work.elapsed_ms << ',' << work.overshoot_ms "
                         "<< ',' << work.attempts << ',' << work.adopted << ',' << work.bytes "
                         "<< ',' << work.byte_overshoot << ',' << pending;")

    def test_deterministic_work_gates_are_registered_on_standard_targets(self):
        cmake = read(ROOT / "test/CMakeLists.txt")
        for target, source in (
            ("common_tests", "common/DeterministicWorkBudget_test.cpp"),
            ("pass_context_test", "rendering/foliage_rebuild_test.cpp"),
        ):
            sources = re.search(r"add_executable\(" + target + r"\s+(.*?)^\)",
                                cmake, re.S | re.M)
            self.assertIsNotNone(sources)
            self.assertIn(source, sources.group(1))
            roster = re.search(r"set\(LUMINUMBRA_GTEST_TARGETS\s+(.*?)^\)",
                               cmake, re.S | re.M)
            self.assertIsNotNone(roster)
            self.assertIn(target, roster.group(1).split())
        native = read(ROOT / "test/common/DeterministicWorkBudget_test.cpp")
        for name in ("EveryPinnedLimitRejectsAnInjectedExcess",
                     "IdleRejectsOneUploadByteAndOneHashByteIndependently",
                     "LiveSubsystemMovementEditAndDrainStayBounded"):
            self.assertIn("TEST(DeterministicWorkBudget, " + name + ")", native)
        for name in ("DeterministicWorkBudgetMovementEditDrainAndIdle",
                     "OutdatedRevisionReadbackIsDiscardedBeforeAdoption"):
            self.assertIn("TEST_F(FoliageRebuildTest, " + name + ")",
                          read(ROOT / "test/rendering/foliage_rebuild_test.cpp"))

    def test_reviewed_work_budget_data_has_integer_units_and_exact_idle_zero(self):
        data = json.loads(read(ROOT / "test/fixtures/deterministic-work-budgets.json"))
        self.assertEqual(data["schema"], "luminumbra.deterministic_work_budgets.v1")
        self.assertEqual(set(data["limits"]), {"idle", "foliage", "simulation"})
        for limits in data["limits"].values():
            self.assertTrue(limits)
            for name, limit in limits.items():
                self.assertIs(type(limit), int)
                self.assertGreaterEqual(limit, 0)
                self.assertLess(limit, 2**64 - 1)
                self.assertNotRegex(name, r"(?i)ms|time|seconds|fps")
        self.assertEqual(data["limits"]["idle"]["uploaded_bytes"], 0)
        self.assertEqual(data["limits"]["idle"]["hashed_bytes"], 0)
        self.assertTrue(all(value == 0 for value in data["limits"]["idle"].values()))

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
