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
                      "foliage_frame.generated_count, foliage_frame.count_generation, phase_scope.snapshot())", main)
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
        self.assertIn("count_generation, phases)",
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
            self.assertIn("shader_first_use_collection_enabled = current_phases != nullptr;",
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
        self.assertTrue(constructor.startswith("if (!enabled || !current_phases) return;"))
        destructor = compact(block(self.collector, "PhaseScope::~PhaseScope()"))
        self.assertTrue(destructor.startswith("if (!m_totals) return;"))
        self.assertIn("m_totals->add(m_phase, elapsed, m_children_ms, foliage - m_child_foliage_ms)",
                      destructor)
        self.assertIn("m_parent->m_children_ms += elapsed;", destructor)
        self.assertIn("m_parent->m_child_foliage_ms += foliage;", destructor)
        rebuild = block(self.foliage, "~RebuildAttributionScope()")
        self.assertIn("Attribution::phase_total_ms() - m_phases_before", rebuild)

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
