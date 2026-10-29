"""Check that presentation integration uses the tested clock-free policy."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]
RENDERING = ROOT / "src/luminumbra_client/rendering"


def read(path):
    return path.read_text(encoding="utf-8")


def compact(text):
    return re.sub(r"\s+", " ", text).strip()


def body(text, signature):
    start = text.index("{", text.index(signature))
    depth = 1
    end = start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start + 1:end - 1]


def brace_depth(text, position):
    # Ignore JSON keys, string contents and comments when checking conditional scope.
    masked = re.sub(
        r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda match: " " * len(match.group()), text, flags=re.S,
    )
    return masked[:position].count("{") - masked[:position].count("}")


class UploadBudgetWiring(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.pipeline = read(RENDERING / "RenderPipeline.cpp")
        cls.header = read(RENDERING / "RenderPipeline.h")
        cls.main = read(ROOT / "src/luminumbra_client/main_client.cpp")

    def test_wiring_regression_runs_in_ctest_and_python_ci(self):
        registration = read(ROOT / "test/CMakeLists.txt")
        self.assertEqual(registration.count("add_test(NAME UploadBudgetWiringPython "), 1)
        self.assertEqual(registration.count("add_executable(upload_budget_test "), 1)
        targets = registration.split("set(LUMINUMBRA_GTEST_TARGETS", 1)[1].split(")", 1)[0]
        self.assertEqual(targets.split().count("upload_budget_test"), 1)
        ci_paths = read(ROOT / "tools/ci/ci_paths.py")
        python_tests = ci_paths.split("PYTHON_TESTS = (", 1)[1].split(")", 1)[0]
        self.assertIn('"test/rendering/test_upload_budget_wiring.py"', python_tests)

    def test_waterfall_replacement_clears_sites_and_spray_before_empty_site_return(self):
        source = read(RENDERING / "passes/WaterfallPass.cpp")
        destroy = compact(body(source, "void WaterfallPass::destroy_geometry()"))
        self.assertIn("m_sites.clear();", destroy)
        self.assertEqual(brace_depth(destroy, destroy.index("m_sites.clear();")), 0)
        build = compact(body(source, "WaterfallPass::build_geometry("))
        self.assertLess(build.index("m_sites.clear();"), build.index("waterfall_sites(world)"))
        begin = compact(body(source, "void WaterfallPass::begin_adoption("))
        self.assertIn("m_replace_spray = true;", begin)
        # Adoption, not preparation, replaces the spray, and it does so even for empty site sets.
        adopt = compact(body(source, "bool WaterfallPass::adopt_next("))
        self.assertIn("(m_sheet_sites.empty() && !m_replace_spray)", adopt)
        replace = adopt.index("if (m_replace_spray && particle_pass)")
        self.assertIn("particle_pass->clear_waterfall_spray();", body(adopt[replace:], "if (m_replace_spray"))
        self.assertLess(replace, adopt.index("particle_pass->add_waterfall_spray("))
        swap = body(self.pipeline, "void RenderPipeline::prepare_world_swap()")
        self.assertIn("m_waterfall_pass->destroy_geometry();", swap)
        self.assertIn("m_particle_pass->clear_waterfall_spray();", swap)
        entry = body(self.main, "auto finish_world_entry =")
        self.assertIn("renderPipeline.prepare_waterfalls(*world_system);", entry)
        forward = body(self.pipeline, "void RenderPipeline::prepare_waterfalls(")
        self.assertIn("m_waterfall_pass->prepare(world, m_particle_pass.get(), m_root_path);",
                      forward)
        preview = read(ROOT / "src/luminumbra_client/world/WorldgenPreview.cpp")
        self.assertIn("pipeline.prepare_world_swap();", preview)

    def test_waterfall_spray_cleanup_preserves_other_emitter_handles_and_indices(self):
        source = read(RENDERING / "passes/ParticlePass.cpp")
        add = body(source, "uint32_t ParticlePass::add_waterfall_spray(")
        self.assertIn("spray.waterfall_spray = true;", add)
        self.assertLess(add.index("if (id == kInvalidEmitter)"),
                        add.index("spray.waterfall_spray = true;"))
        clear = compact(body(source, "void ParticlePass::clear_waterfall_spray()"))
        self.assertIn("std::array<uint32_t, kMaxEmitters> remap{};", clear)
        self.assertIn("m_active_emitters[index].waterfall_spray ? kInvalidEmitter : retained++", clear)
        self.assertIn("particle.alive = false;", clear)
        self.assertIn("m_live_count = 0;", clear)
        self.assertIn("++m_live_count;", clear)
        self.assertIn("particle.emitter_index = replacement;", clear)
        self.assertIn("m_splash_emitter_index = remap[m_splash_emitter_index];", clear)
        self.assertIn("m_ambient_emitter_index = remap[m_ambient_emitter_index];", clear)
        self.assertIn("std::erase_if(m_active_emitters,", clear)
        self.assertIn("m_descriptors.clear();", clear)
        self.assertIn("m_frame_instance_count = 0;", clear)
        self.assertNotIn("clear_emitters()", clear)
        self.assertNotIn("m_next_emitter_id =", clear)
        native = read(ROOT / "test/rendering/particle_determinism_test.cpp")
        self.assertIn("WaterfallReplacementPreservesOtherEmitterHandles", native)
        self.assertIn("particle_determinism_test", read(ROOT / "test/CMakeLists.txt"))

    def test_both_live_backends_use_policy_and_include_preparation(self):
        self.assertIn("manage_mesh_gpu_resources<false>", self.pipeline)
        self.assertIn("manage_mesh_gpu_resources<true>", self.pipeline)
        integrate = body(self.pipeline, "void RenderPipeline::manage_mesh_gpu_resources(")
        self.assertLess(integrate.index("steady_clock::now()"), integrate.index("active.reserve"))
        self.assertLess(integrate.index("steady_clock::now()"), integrate.index("refresh_uploads("))
        self.assertIn("preparation_ms +", integrate)
        self.assertRegex(integrate, r"drain_uploads\(\s*queue,")
        self.assertIn("copy_terrain_mesh_payload(candidate.chunk, payload)", integrate)
        self.assertIn("copy_water_mesh_payload(candidate.chunk, payload)", integrate)
        self.assertIn("upload_chunk_mesh(candidate.chunk, payload)", integrate)
        self.assertIn("upload_water_mesh(candidate.chunk, payload)", integrate)
        self.assertIn("record_render_uploads(work, Water, pending)", integrate)
        self.assertNotIn("kMinTerrainUploadsPerFrame", self.pipeline)
        upload = body(self.pipeline, "void RenderPipeline::upload_chunk_mesh(")
        self.assertIn("m_foliage_pass->update_ground_mesh(", upload)
        self.assertIn("manage_chunk_gpu_resources(renderable_chunk_snapshots, camera, snapshot_ms)",
                      self.pipeline)
        self.assertLess(self.pipeline.index("const auto snapshot_start"),
                        self.pipeline.index("build_chunk_snapshots(renderable_chunks)"))

    def test_both_lanes_sort_only_arrivals_before_budget_admission(self):
        integrate = body(self.pipeline, "void RenderPipeline::manage_mesh_gpu_resources(")
        self.assertLess(integrate.index("refresh_uploads("), integrate.index("drain_uploads("))
        self.assertNotIn("order_uploads(candidates", integrate)
        scheduler = read(RENDERING / "UploadBudget.h")
        refresh = compact(body(scheduler, "void refresh_uploads("))
        self.assertIn("const std::vector<Item>& candidates", scheduler)
        self.assertIn("std::vector<const Item*> arrivals;", refresh)
        self.assertIn("if (queue.contains(id(candidate))) queue.refresh(id(candidate), candidate); "
                      "else arrivals.push_back(&candidate);", refresh)
        self.assertIn("if (arrivals.size() > 1)", refresh)
        self.assertEqual(refresh.count("order_uploads(arrivals,"), 2)
        self.assertIn("return id(*item);", refresh)
        self.assertIn("return priority(*item);", refresh)
        self.assertLess(refresh.index("queue.begin_refresh()"), refresh.index("queue.contains("))
        self.assertLess(refresh.index("queue.refresh(id(*arrival), *arrival)"),
                        refresh.index("queue.end_refresh()"))
        native = read(ROOT / "test/rendering/upload_budget_test.cpp")
        for name in ("RetainedBacklogIsRefreshedWithoutRepeatedPriorityWork",
                     "OnlyArrivalsAreSortedAndTheyFollowRetainedWork"):
            self.assertIn("TEST(UploadBudget, " + name + ")", native)

    def test_sizes_are_checked_before_copying_in_both_lanes(self):
        for lane in ("terrain", "water"):
            copy = body(self.pipeline, f"bool RenderPipeline::copy_{lane}_mesh_payload(")
            self.assertLess(copy.index("mesh_upload_bytes("), copy.index("payload.vertices ="))
            self.assertLess(copy.index("mesh_upload_bytes("), copy.index("payload.indices ="))

    def test_unmeasured_benchmark_waits_drain_before_drawing(self):
        prepare = body(self.pipeline, "void RenderPipeline::prepare_frame(")
        guard = "if (!draw_upload_frame(presentation, render_uploads_settled()))"
        self.assertLess(prepare.index("m_frame_prepared.valid = false;"),
                        prepare.index("manage_chunk_gpu_resources("))
        self.assertLess(prepare.index("manage_water_gpu_resources("), prepare.index(guard))
        self.assertLess(prepare.index(guard), prepare.index("ensure_terrain_culling_hierarchy("))
        self.assertLess(prepare.index(guard), prepare.index("AdvanceSelectedFarWorld("))
        self.assertIn("return;", body(prepare, guard))
        render = body(self.pipeline, "void RenderPipeline::render_frame(")
        self.assertLess(render.index("if (!m_frame_prepared.valid)"),
                        render.index("dispatch_stages(camera)"))
        self.assertIn("UploadPresentation presentation = UploadPresentation::Streaming",
                      compact(self.header))
        # Evaluate the shipping selector for all phase boundaries. A measured
        # frame must never receive the settled-only drawing policy.
        expression = re.search(r"const bool settling_benchmark_uploads = (.*?);",
                               compact(self.main)).group(1)
        names = {
            "g_app.benchmark.warmupFrames": "warm",
            "g_app.capture.render_benchmark_warmup": "minimum",
            "g_app.benchmark.measuredFrames": "measured",
            "g_app.capture.render_benchmark_frames": "total",
        }
        for source, target in names.items():
            expression = expression.replace(source, target)
        expression = expression.replace("&&", " and ").replace("||", " or ")
        expression = expression.replace("!", "not ")
        for active in (False, True):
            for minimum in (0, 1, 2, 60):
                for warm in range(minimum + 1):
                    for measured in (0, 3, 4):
                        complete = warm >= minimum
                        actual = eval(expression, {"__builtins__": {}}, dict(
                            g_rb_active=active, upload_warmup_complete=complete,
                            warm=warm, minimum=minimum, measured=measured, total=4))
                        expected = active and ((not complete and warm + 1 >= minimum)
                                               or measured >= 4)
                        self.assertEqual(actual, expected)
        call = compact(self.main[self.main.index("const bool settling_benchmark_uploads"):])
        self.assertRegex(call, r"renderPipeline.render_frame\(.*?settling_benchmark_uploads "
                         r"\|\| settling_capture_uploads "
                         r"\? Luminumbra::Rendering::UploadPresentation::Settled "
                         r": Luminumbra::Rendering::UploadPresentation::Streaming\)")
        native = read(ROOT / "test/rendering/upload_budget_test.cpp")
        for name in ("SlowPreparationDrainsWithoutDrawingEveryIntermediateImage",
                     "FailedQualificationNeverDrawsAnUnsettledImage"):
            self.assertIn("TEST(UploadBudget, " + name + ")", native)
        integration = read(ROOT / "test/rendering/pass_context_test.cpp")
        self.assertIn("TEST(PassContext, QualificationUploadsDrainBeforeDispatchAndTemporalHistory)",
                      integration)
        slow = body(self.main, "if (_wallMs > 12.0")
        for lane in ("terrain", "water"):
            for field in ("pending={}", "adopted={}", "failures={}", "ms={:.1f}"):
                self.assertIn(lane + "_" + field, slow)
            self.assertIn(f"slow_uploads.{lane}_uploads_deferred", slow)
            self.assertIn(f"slow_uploads.{lane}_upload_failures", slow)
            self.assertIn(f"slow_uploads.{lane}_budget.adopted", slow)
            self.assertIn(f"slow_uploads.{lane}_budget.elapsed_ms", slow)

    def test_shipping_draw_policy_omits_backlog_images_and_refuses_unsettled_output(self):
        scheduler = read(RENDERING / "UploadBudget.h")
        expression = re.search(r"return (.*?);", body(scheduler, "constexpr bool draw_upload_frame(")).group(1)
        expression = expression.replace("UploadPresentation::Streaming", repr("Streaming"))
        expression = expression.replace("||", " or ").replace("&&", " and ")
        expression = expression.replace("true", "True").replace("false", "False")

        def draws(presentation, settled):
            return eval(expression, {"__builtins__": {}}, dict(
                presentation=presentation, settled=settled))

        # With preparation over budget, the unchanged progress floor permits
        # one adoption per pass. Run the shipping draw predicate at every pass.
        for presentation, expected in (("Streaming", 512), ("Settled", 1)):
            self.assertEqual(sum(draws(presentation, remaining == 0)
                                 for remaining in range(511, -1, -1)), expected)
            self.assertTrue(draws(presentation, True))  # empty backlog
        self.assertFalse(draws("Settled", False))  # failed or partial adoption
        self.assertTrue(draws("Streaming", False))  # measured streaming frame

    def test_capture_wait_siblings_defer_only_after_required_settling_frames(self):
        expression = re.search(r"const bool settling_capture_uploads = (.*?);",
                               compact(self.main)).group(1)
        names = {
            "g_app.capture.scene_active": "scene",
            "g_app.capture.render_benchmark_path.empty()": "no_benchmark",
            "g_app.sceneCapture.settleFrames": "scene_frames",
            "g_app.capture.survey_active": "survey",
            "g_app.capture.survey_settle": "survey_frames",
            "g_app.capture.frame_scan_active": "scan",
            "g_app.capture.frame_scan_settle_target": "scan_target",
            "g_app.capture.frame_scan_settle": "scan_frames",
        }
        for source, target in names.items():
            expression = expression.replace(source, target)
        expression = expression.replace("&&", " and ").replace("||", " or ")
        for scene in (False, True):
            for survey in (False, True):
                for scan in (False, True):
                    for no_benchmark in (False, True):
                        for scene_frames in (0, 54, 55, 56):
                            for survey_frames in (0, 44, 45, 46):
                                for scan_frames in (0, 59, 60, 61):
                                    values = dict(scene=scene, survey=survey, scan=scan,
                                                  no_benchmark=no_benchmark,
                                                  scene_frames=scene_frames,
                                                  survey_frames=survey_frames,
                                                  scan_frames=scan_frames, scan_target=60)
                                    expected = ((scene and no_benchmark and scene_frames >= 55)
                                                or (survey and survey_frames >= 45)
                                                or (scan and scan_frames >= 60))
                                    self.assertEqual(eval(expression, {"__builtins__": {}}, values),
                                                     expected)
        for path in (RENDERING / "SceneSurvey.cpp",
                     ROOT / "src/luminumbra_client/core/ScenarioRunnerCapture.cpp"):
            drain = read(path)
            drain = drain[drain.index("settle_uploads("):]
            self.assertIn("UploadPresentation::Settled", drain.split("return false;", 1)[0])

    def test_residency_retries_freeze_sun_and_cloud_elapsed_time(self):
        # Check the actual render arguments in both synchronous drain callbacks.
        for path in (RENDERING / "SceneSurvey.cpp",
                     ROOT / "src/luminumbra_client/core/ScenarioRunnerCapture.cpp"):
            with self.subTest(path=path.name):
                source = compact(read(path))
                drain = source[source.index("settle_uploads("):]
                calls = re.findall(r"\.render_frame\((.*?)\);", drain)
                self.assertEqual(len(calls), 1)
                arguments = [argument.strip() for argument in calls[0].split(",")]
                self.assertEqual(len(arguments), 6)
                self.assertEqual(arguments[3], "0.0f")
                self.assertTrue(arguments[5].endswith("UploadPresentation::Settled"))

        # The main loop uses the same wait selector for elapsed time and drawing.
        source = compact(self.main)
        wait = source[source.index("const bool settling_benchmark_uploads"):]
        call = re.search(r"\.render_frame\((.*?)\);", wait).group(1)
        arguments = [argument.strip() for argument in call.split(",")]
        self.assertEqual(len(arguments), 6)
        self.assertEqual(arguments[3], "settling_benchmark_uploads || "
                         "settling_capture_uploads ? 0.0f : deltaTime")
        self.assertEqual(arguments[5], "settling_benchmark_uploads || "
                         "settling_capture_uploads "
                         "? Luminumbra::Rendering::UploadPresentation::Settled "
                         ": Luminumbra::Rendering::UploadPresentation::Streaming")

    def test_retention_uses_pool_transaction_and_water_staging(self):
        pool = read(RENDERING / "passes/ChunkGeometryPool.cpp")
        update = body(pool, "u32 ChunkGeometryPool::update(")
        self.assertLess(update.index("allocate_with_reclaim("),
                        update.index("m_allocations[handle].live = false"))
        self.assertIn("capacity_refused ? reuse_completed(", compact(update))
        water = body(self.pipeline, "void RenderPipeline::upload_water_mesh(")
        self.assertIn("WaterRenderData data;", water)
        self.assertLess(water.index("if (!vertex_uploaded"), water.index("it->second = data;"))
        self.assertIn("!vertex_uploaded || !index_uploaded || !attributes_valid", water)
        self.assertIn("delete_water_slot(data);", water)
        draw = body(self.pipeline, "void RenderPipeline::execute_stage_water(")
        self.assertIn("PresentedLadderWaterEligible(", draw)
        self.assertIn("water_data.SetPresentedLadderWaterUpload(", self.pipeline)
        presentation = body(self.header, "void SetPresentedLadderWaterUpload(")
        self.assertIn("SetLadderWaterUpload(upload, mesh_version, enabled)", presentation)

    def test_settled_capture_and_per_frame_telemetry_are_both_wired(self):
        settled = body(self.header, "bool render_uploads_settled() const")
        self.assertIn("terrain_uploads_deferred == 0", settled)
        self.assertIn("water_uploads_deferred == 0", settled)
        for wait in ("scene_upload_wait", "survey_upload_wait", "scan_upload_wait",
                     "warmup_upload_wait", "capture_upload_wait"):
            self.assertTrue(re.search(r"wait_for_uploads\(\s*" + wait + ",", self.main), wait)
        wait = body(self.main, "const auto wait_for_uploads =")
        self.assertIn("wait.waiting(pending,", wait)
        self.assertIn("wait.failed()", wait)
        self.assertIn('"status", "failed"', wait)
        self.assertIn('"capture_complete", false', wait)
        self.assertIn("exit_code = 2", wait)
        self.assertIn("glfwSetWindowShouldClose(window, GLFW_TRUE)", wait)
        self.assertIn("terrain_coverage_frames.size() <", self.main)
        self.assertIn("record_warmup || record_measured || record_capture", self.main)
        self.assertIn("const bool record_measured = upload_warmup_complete", compact(self.main))
        self.assertIn("if (!upload_warmup_complete)", self.main)
        self.assertIn(
            "terrain_coverage_frames.size() < "
            "static_cast<std::size_t>(g_app.capture.render_benchmark_warmup) + "
            "static_cast<std::size_t>(g_app.capture.render_benchmark_frames) + 1",
            compact(self.main))
        coverage = read(RENDERING / "TerrainCoverageCapture.cpp")
        capture = body(coverage, "bool RenderPipeline::capture_terrain_coverage(")
        self.assertIn("!render_uploads_settled()", capture)
        attribution = read(ROOT / "src/luminumbra_client/app/FrameAttribution.cpp")
        for column in ("terrain_work_ms", "terrain_overshoot_ms", "terrain_byte_overshoot",
                       "terrain_pending", "water_work_ms", "water_overshoot_ms", "water_pending"):
            self.assertIn(column, attribution)
        self.assertIn("foliage_frame.uploads", self.main)
        for capture in (RENDERING / "SceneSurvey.cpp",
                        ROOT / "src/luminumbra_client/core/ScenarioRunnerCapture.cpp"):
            source = read(capture)
            self.assertIn("settle_uploads(", source)
            self.assertIn("pending.terrain_uploads_deferred", source)
            self.assertIn("pending.water_uploads_deferred", source)

    def test_default_coverage_emits_every_upload_field_before_chunk_detail_return(self):
        diagnostics = body(read(RENDERING / "TerrainCoverageCapture.cpp"),
                           "nlohmann::json RenderPipeline::terrain_coverage_diagnostics(")
        guard = "if (!include_live_chunks || !m_frame_prepared.valid)"
        prefix, detail = diagnostics.split(guard, 1)
        condition = guard[len("if ("):-1].replace("!", "not ").replace("||", " or ")
        for include_live_chunks in (False, True):
            for valid in (False, True):
                with self.subTest(include_live_chunks=include_live_chunks, valid=valid):
                    actual = eval(condition.replace("m_frame_prepared.valid", "valid"),
                                  {"__builtins__": {}},
                                  dict(include_live_chunks=include_live_chunks, valid=valid))
                    self.assertEqual(actual, not include_live_chunks or not valid)
        self.assertIn("bool include_live_chunks = false", self.header)
        self.assertIn("renderPipeline.terrain_coverage_diagnostics();", self.main)
        self.assertIn("const auto upload_work =", prefix)
        serializer = body(prefix, "const auto upload_work =")
        fields = ("attempts", "adopted", "bytes", "byte_overshoot", "elapsed_ms", "overshoot_ms")
        self.assertEqual(dict(re.findall(r'\{"(\w+)", work\.(\w+)\}', serializer)),
                         {field: field for field in fields})
        assignments = ['out["live"]["render_uploads_settled"] = render_uploads_settled();']
        for lane in ("terrain", "water"):
            assignments.append(f'out["live"]["{lane}_upload_budget"] = '
                               f'upload_work(uploads.{lane}_budget);')
        for assignment in assignments:
            self.assertIn(assignment, prefix)
            self.assertEqual(brace_depth(prefix, prefix.index(assignment)), 0)
        streaming = 'out["live"]["streaming"] ='
        self.assertEqual(brace_depth(prefix, prefix.index(streaming)), 0)
        pending = body(prefix, streaming)
        for lane in ("terrain", "water"):
            self.assertIn(f'"{lane}_uploads_pending", uploads.{lane}_uploads_deferred', pending)
        # Only the explicit disabled-diagnostics contract can return before these fields.
        self.assertEqual(prefix.count("return out;"), 1)
        self.assertIn("if (!m_terrain_coverage_enabled) return out;", compact(prefix))
        self.assertTrue(compact(detail).startswith("return out;"))
        self.assertNotIn("renderable_chunk_snapshots", prefix)
        self.assertIn("renderable_chunk_snapshots", detail)

    def test_upload_summaries_survive_details_and_unknown_world_metadata(self):
        diagnostics = body(read(RENDERING / "TerrainCoverageCapture.cpp"),
                           "nlohmann::json RenderPipeline::terrain_coverage_diagnostics(")
        metadata_guard = ("if (const auto* world = m_frame_prepared.world_system; "
                          "m_frame_prepared.valid && world)")
        self.assertIn(metadata_guard, diagnostics)
        metadata = body(diagnostics, metadata_guard)
        self.assertIn('out["live"]["streaming"].update(', metadata)
        for field in ("requested_radius_chunks", "target_radius_chunks",
                      "generation_pending", "meshing_pending"):
            self.assertIn('"' + field + '"', metadata)
        self.assertNotIn("uploads.", metadata)
        condition = metadata_guard.split(";", 1)[1].rstrip(")").strip()
        condition = condition.replace("m_frame_prepared.valid", "valid").replace("&&", " and ")
        for valid in (False, True):
            for world in (False, True):
                with self.subTest(valid=valid, world=world):
                    self.assertEqual(eval(condition, {"__builtins__": {}},
                                          dict(valid=valid, world=world)), valid and world)
        self.assertIn('out["live"].update(', diagnostics)
        self.assertNotRegex(diagnostics, r'out\["live"\]\s*=')
        # Neither the optional metadata nor the chunk list may replace the pending counts.
        self.assertEqual(diagnostics.count('out["live"]["streaming"] ='), 1)
        self.assertLess(diagnostics.index('out["live"]["streaming"] ='),
                        diagnostics.index(metadata_guard))

    def test_both_benchmark_schemas_emit_uploads_without_camera_metadata(self):
        report = body(self.main, "else if (wait_for_uploads(capture_upload_wait,")
        camera_guard = "if (g_camera && gameSession && gameSession->GetWorldSystem())"
        prefix = report.split(camera_guard, 1)[0]
        self.assertIn('j["schema"] = "luminumbra.render_benchmark.v2";', prefix)
        self.assertIn('j["schema"] = "luminumbra.render_benchmark.v3";',
                      body(report, "if (benchmark_v3)"))
        self.assertIn('j["upload_settling_frames"] = {{"warmup", upload_warmup_wait_frames}, '
                      '{"capture", upload_capture_wait_frames}};', compact(prefix))
        self.assertEqual(brace_depth(prefix, prefix.index('j["upload_settling_frames"] =')), 0)
        for lane in ("terrain", "water"):
            for field in ("uploads_pending", "upload_budget"):
                assignment = f'j["streaming"]["{lane}_{field}"] ='
                self.assertIn(assignment, prefix)
                self.assertEqual(brace_depth(prefix, prefix.index(assignment)), 0)
            self.assertIn(f'uploads.{lane}_uploads_deferred;', prefix)
            self.assertIn(f'uploads.{lane}_budget, '
                          f'Luminumbra::Rendering::k{lane.title()}UploadLimits', prefix)
        settled = 'j["streaming"]["render_uploads_settled"] ='
        self.assertIn(settled + ' renderPipeline.render_uploads_settled();', prefix)
        self.assertEqual(brace_depth(prefix, prefix.index(settled)), 0)
        serializer = body(prefix, "const auto upload_observation =")
        fields = ("attempts", "adopted", "bytes", "byte_overshoot", "elapsed_ms", "overshoot_ms")
        self.assertEqual(dict(re.findall(r'\{"(\w+)", work\.(\w+)\}', serializer)),
                         {field: field for field in fields})
        self.assertEqual(dict(re.findall(r'\{"(\w+)", limits\.(\w+)\}', serializer)),
                         {"byte_limit": "bytes", "count_limit": "count",
                          "progress_floor": "progress", "time_limit_ms": "milliseconds"})
        aggregate = prefix[prefix.index("const auto& uploads ="):]
        self.assertNotIn("return", aggregate.replace(serializer, ""))
        self.assertNotRegex(report, r'j\["streaming"\]\s*=')
        self.assertIn('j["streaming"].update(', body(report, camera_guard))

    def test_empty_water_replacements_are_adopted_through_the_budget(self):
        integrate = body(self.pipeline, "void RenderPipeline::manage_mesh_gpu_resources(")
        self.assertIn("!Water || found == records.end() || found->second.element_count == 0", integrate)
        self.assertIn("!chunk.water_metadata_valid", integrate)
        self.assertIn("if (payload.has_mesh())", integrate)
        self.assertIn("unload_water_resources(candidate.chunk.id)", integrate)
        copy = body(self.pipeline, "bool RenderPipeline::copy_water_mesh_payload(")
        self.assertNotIn("!chunk.has_water_mesh()", copy)
        self.assertIn("version_before != chunk.water_mesh_version", copy)
        self.assertIn("version_after != version_before", copy)

    def test_drain_accounts_for_arrivals_and_all_capture_callers_report_failure(self):
        scheduler = read(RENDERING / "UploadBudget.h")
        drain = body(scheduler, "bool settle_uploads(")
        self.assertIn("UploadSettlingWait wait", drain)
        self.assertIn("wait.waiting(work.first, work.second)", drain)
        self.assertIn("return !wait.failed()", drain)
        self.assertIn("kArrivalAllowance - m_arrivals", scheduler)
        for path in (RENDERING / "SceneSurvey.cpp",
                     ROOT / "src/luminumbra_client/core/ScenarioRunnerCapture.cpp"):
            source = read(path)
            self.assertIn("pending.terrain_budget.adopted + pending.water_budget.adopted", compact(source))
            self.assertIn("pending.terrain_uploads_deferred + pending.water_uploads_deferred", compact(source))
            self.assertRegex(source, r"\)\)\s*(?:\{[\s\S]*?)?return false;")
        self.assertIn("if (!Luminumbra::Rendering::RunSceneSurvey(", self.main)
        self.assertIn('"reason", "survey_capture_incomplete"', self.main)

    def test_registered_gl_regressions_use_the_production_pool(self):
        tests = read(ROOT / "test/rendering/FarWorldWaterGl_test.cpp")
        registration = read(ROOT / "test/CMakeLists.txt")
        self.assertIn("rendering/FarWorldWaterGl_test.cpp", registration)
        for operation in ("pool.allocate(", "pool.update(", "pool.allocation(",
                          "pool.destroy()", "glGetBufferSubData(", "glIsBuffer("):
            self.assertIn(operation, tests)
        self.assertIn("FailedPoolBlock failure", tests)
        self.assertIn("WetToDryAdoptsRemovalInBothWaterModes", tests)
        pool = read(RENDERING / "passes/ChunkGeometryPool.cpp")
        allocation = body(pool, "u32 ChunkGeometryPool::allocate_with_reclaim(")
        self.assertLess(allocation.index("block_index == kInvalid)\n            return kInvalid"),
                        allocation.index("std::memcpy("))

    def test_terrain_retirement_covers_every_submission_and_never_overwrites(self):
        pool = read(RENDERING / "passes/ChunkGeometryPool.cpp")
        pool_header = read(RENDERING / "passes/ChunkGeometryPool.h")
        update = body(pool, "u32 ChunkGeometryPool::update(")
        self.assertNotIn("std::memcpy", update)
        self.assertNotIn("return handle;", update)
        self.assertIn("allocate_with_reclaim(", update)
        self.assertIn("capacity_refused ? reuse_completed(", compact(update))
        self.assertLess(update.index("if (graphics_failed ||"),
                        update.index("m_allocations[handle].live = false;"))
        release = compact(body(pool, "void ChunkGeometryPool::free("))
        self.assertIn("alloc.live = false;", release)
        self.assertEqual(release.count("recycle(handle)"), 1)
        self.assertLess(release.index("if (!m_blocks[alloc.block_index].submitted)"),
                        release.index("recycle(handle)"))
        self.assertIn("if (!m_blocks[alloc.block_index].submitted)", release)
        self.assertIn("m_retired.push_back({handle, glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)})", release)
        self.assertNotIn("free_vertex_slices", release)
        self.assertIn("reclaim(kTerrainUploadLimits.count)",
                      body(pool, "void ChunkGeometryPool::reclaim()"))
        reclaim = compact(body(pool, "void ChunkGeometryPool::reclaim(std::size_t limit)"))
        self.assertIn("std::min(limit, m_retired.size())", reclaim)
        self.assertIn("count < visits", reclaim)
        self.assertIn("if (!retired.completion) { m_retired.push_back(retired); continue; }", reclaim)
        self.assertIn("glClientWaitSync(retired.completion, GL_SYNC_FLUSH_COMMANDS_BIT, 0)", reclaim)
        self.assertIn("if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) "
                      "{ m_retired.push_back(retired); continue; }", reclaim)
        self.assertLess(reclaim.index("GL_CONDITION_SATISFIED"), reclaim.index("recycle(retired.handle)"))
        self.assertIn("m_retired.pop_front()", reclaim)
        allocation = compact(body(pool, "u32 ChunkGeometryPool::allocate_with_reclaim("))
        self.assertIn("reclaim(visits);", allocation)
        self.assertIn("m_allocations.size() >= kMaxAllocations", allocation)
        add = compact(body(pool, "u32 ChunkGeometryPool::add_block("))
        self.assertIn("m_blocks.size() >= kMaxBlocks", add)
        self.assertIn("checked_bytes > kMaxStorageBytes - m_storage_bytes", add)
        self.assertIn("kMaxStorageBytes = 512u * 1024u * 1024u", pool_header)
        self.assertIn("kMaxBlocks = 16", pool_header)
        self.assertIn("kMaxAllocations = 65536", pool_header)
        self.assertNotIn("glFinish(", pool)
        destroy = body(pool, "void ChunkGeometryPool::destroy()")
        self.assertNotIn("glUnmapBuffer", destroy)
        self.assertIn("glDeleteSync(retired.completion)", destroy)
        self.assertIn("m_storage_bytes = 0", destroy)
        draw = body(self.pipeline, "void RenderPipeline::draw_chunks_mdi(")
        self.assertIn("m_chunk_geometry_pool.mark_submitted(", draw)
        self.assertLess(compact(draw).index("AttributedGl::Submit( glMultiDrawElementsIndirect,"),
                        draw.index("m_chunk_geometry_pool.mark_submitted("))
        self.assertEqual(compact(self.pipeline).count("AttributedGl::Submit( glMultiDrawElementsIndirect,"), 1)
        probe = read(ROOT / "test/rendering/render_smoke_test.cpp")
        self.assertIn("glMultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, nullptr, 1, 0);\n"
                      "        pool_.mark_submitted(allocation.block_index);", probe)
        self.assertEqual(draw.count("valid_allocation(rd.pool_handle, rd.element_count)"), 2)
        native = read(ROOT / "test/rendering/FarWorldWaterGl_test.cpp")
        for name in ("DelayedConsumerPreventsOverwriteAndSliceReuseUntilCompletion",
                     "MissingFenceQuarantinesStorageAndRetirementPollingIsBounded"):
            self.assertIn("TEST_F(FarWorldWaterGl, " + name + ")", native)
        self.assertIn("glCopyBufferSubData(", native)
        self.assertEqual(self.pipeline.count("m_chunk_geometry_pool.destroy()"), 1)
        clear = body(self.pipeline, "void RenderPipeline::clear_all_chunk_data()")
        self.assertIn("m_chunk_geometry_pool.clear()", clear)
        self.assertNotIn("m_chunk_geometry_pool.destroy()", clear)
        world_clear = compact(body(pool, "void ChunkGeometryPool::clear()"))
        self.assertIn("free(handle)", world_clear)
        self.assertNotIn("m_storage_bytes =", world_clear)
        self.assertNotIn("m_retired.clear()", world_clear)

    def test_pool_retirement_visits_are_bounded_and_keep_quarantined_handles(self):
        pool = read(RENDERING / "passes/ChunkGeometryPool.cpp")
        reclaim = compact(body(pool, "void ChunkGeometryPool::reclaim(std::size_t limit)"))
        self.assertEqual(reclaim.count("glClientWaitSync("), 1)
        self.assertEqual(reclaim.count("for ("), 1)
        self.assertNotIn("while (", reclaim)
        self.assertNotIn("break;", reclaim)
        self.assertEqual(reclaim.count("m_retired.push_back(retired)"), 2)
        self.assertLess(reclaim.index("m_retired.pop_front()"),
                        reclaim.index("if (!retired.completion)"))
        self.assertLess(reclaim.index("GL_CONDITION_SATISFIED"),
                        reclaim.index("glDeleteSync(retired.completion)"))
        self.assertLess(reclaim.index("glDeleteSync(retired.completion)"),
                        reclaim.index("recycle(retired.handle)"))
        self.assertEqual(reclaim.count("recycle(retired.handle)"), 1)
        native = read(ROOT / "test/rendering/FarWorldWaterGl_test.cpp")
        for name in ("FailedOrIncompleteFrontDoesNotBlockHealthyRetirement",
                     "NullFrontDoesNotBlockHealthyRetirement"):
            self.assertIn("TEST_F(FarWorldWaterGl, " + name + ")", native)
        progress = body(native, "void CheckHealthyRetirementBehindBlockedFront(")
        for assertion in ("EXPECT_EQ(pool.retired_count(), 4u)",
                          "EXPECT_EQ(pool.retired_count(), 1u)",
                          "EXPECT_EQ(completion.polls - polls_before_upload, missing ? 3u : 4u)",
                          "EXPECT_NE(replacement, handles.front())",
                          "EXPECT_EQ(consumer.Read(), glm::vec3(1, 2, 3))"):
            self.assertIn(assertion, progress)

    def test_reclaimed_blocks_reset_only_after_all_consumers_release(self):
        pool = read(RENDERING / "passes/ChunkGeometryPool.cpp")
        allocation = compact(body(pool, "u32 ChunkGeometryPool::allocate_with_reclaim("))
        recycle = compact(body(pool, "void ChunkGeometryPool::recycle("))
        self.assertIn("++block.allocation_count;", allocation)
        self.assertIn("if (--block.allocation_count == 0) { block.vertex_high_water = 0; "
                      "block.index_high_water = 0; block.free_vertex_slices.clear(); "
                      "block.free_index_slices.clear();", recycle)
        self.assertIn("} else { block.free_vertex_slices.emplace_back", recycle)
        self.assertEqual(recycle.count("--block.allocation_count"), 1)
        self.assertLess(recycle.index("--block.allocation_count"),
                        recycle.index("release_handle(handle)"))
        add = compact(body(pool, "u32 ChunkGeometryPool::add_block("))
        self.assertIn("if (candidate.allocation_count != 0) continue;", add)
        self.assertIn("checked_bytes > kMaxStorageBytes - m_storage_bytes + reclaimable_bytes", add)
        self.assertIn("if (candidate.allocation_count == 0) release_block(candidate);", add)
        self.assertIn("release_block(m_blocks[block_index])", add)
        self.assertIn("m_blocks[block_index] = std::move(block)", add)
        self.assertNotIn("m_blocks.erase", pool)
        release = compact(body(pool, "void ChunkGeometryPool::release_block("))
        self.assertIn("m_storage_bytes -=", release)
        self.assertIn("block = Block{}", release)
        for field in ("vao", "vbo", "ebo"):
            self.assertIn("&block." + field, release)
        native = read(ROOT / "test/rendering/FarWorldWaterGl_test.cpp")
        for name in ("FullyReclaimedFragmentedWorldAdmitsLargerMeshInSameBlock",
                     "FailedReservationLeavesReclaimedFrontiersUnchanged",
                     "PendingWorldSwapRetainsStorageAndBytesUntilCompletion",
                     "EmptyBlockReplacementPreservesQuarantinedAndLiveBlockIndices",
                     "StorageCapIncludesPendingWorldsAndReleasesOnlyCompletedEmptyBlocks"):
            self.assertIn("TEST_F(FarWorldWaterGl, " + name + ")", native)

    def test_pool_reservations_commit_both_buffers_together(self):
        pool = read(RENDERING / "passes/ChunkGeometryPool.cpp")
        reserve = compact(body(pool, "bool ChunkGeometryPool::reserve_in_block("))
        decision = reserve.index("if (!find_slice(block.free_index_slices,")
        for mutation in ("block.free_vertex_slices.pop_back()",
                         "block.free_index_slices.pop_back()",
                         "block.vertex_high_water += v_slot",
                         "block.index_high_water += i_slot"):
            self.assertGreater(reserve.index(mutation), decision)
        self.assertNotIn("emplace_back", reserve)

    def test_pool_capacity_fallback_observes_final_consumer_before_both_writes(self):
        pool = read(RENDERING / "passes/ChunkGeometryPool.cpp")
        update = compact(body(pool, "u32 ChunkGeometryPool::update("))
        self.assertIn("label_seed, kTerrainUploadLimits.count - 1, capacity_refused)", update)
        self.assertIn("return capacity_refused ? reuse_completed(handle, vertices, vertex_count, "
                      "indices, index_count) : kInvalid;", update)
        allocation = compact(body(pool, "u32 ChunkGeometryPool::allocate_with_reclaim("))
        self.assertEqual(allocation.count("pool_gl_error()"), 2)
        self.assertLess(allocation.index("pool_gl_error()"), allocation.index("reclaim(visits)"))
        self.assertLess(allocation.rindex("pool_gl_error()"), allocation.index("reserve_in_block("))
        self.assertEqual(allocation.count("if (pool_gl_error()) return kInvalid;"), 2)
        reuse = compact(body(pool, "u32 ChunkGeometryPool::reuse_completed("))
        self.assertIn("new_slot == old_slot ||", reuse)
        self.assertIn("offset + old_slot == frontier && new_slot <= capacity - offset", reuse)
        for decision in ("fits(alloc.vertex_offset,", "fits(alloc.index_offset,",
                         "!complete_for_reuse(block)"):
            self.assertIn(decision, reuse)
            self.assertLess(reuse.index(decision), reuse.index("std::memcpy("))
        self.assertIn("block.vertex_capacity) || !fits(alloc.index_offset,", reuse)
        self.assertIn("block.index_capacity) || !complete_for_reuse(block)", reuse)
        self.assertEqual(reuse.count("std::memcpy("), 2)
        for mutation in ("alloc.vertex_slot =", "alloc.index_slot =", "alloc.vertex_count =",
                         "alloc.index_count =", "block.vertex_high_water +=",
                         "block.index_high_water +="):
            self.assertLess(reuse.rindex("std::memcpy("), reuse.index(mutation))
        # No fallible operation is allowed between the first write and publication.
        commit = reuse[reuse.index("std::memcpy("):]
        self.assertNotRegex(commit, r"\bgl[A-Z]|return kInvalid|allocate\(|push_back|emplace_back")
        self.assertIn("alloc.vertex_count = vertex_count;", commit)
        self.assertIn("alloc.index_count = index_count;", commit)
        self.assertIn("return handle;", commit)
        completion = compact(body(pool, "bool ChunkGeometryPool::complete_for_reuse("))
        self.assertIn("if (block.reuse_quarantined) return false;", completion)
        self.assertIn("glClientWaitSync(block.reuse_completion, GL_SYNC_FLUSH_COMMANDS_BIT, 0)",
                      completion)
        self.assertIn("graphics_failed || status == GL_WAIT_FAILED", completion)
        self.assertIn("status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED", completion)
        self.assertEqual(completion.count("glClientWaitSync("), 1)
        self.assertLess(completion.index("glDeleteSync("), completion.rindex("pool_gl_error()"))
        self.assertLess(completion.rindex("pool_gl_error()"), completion.rindex("return true;"))
        submitted = compact(body(pool, "void ChunkGeometryPool::mark_submitted("))
        self.assertIn("if (block.reuse_requested && !block.reuse_quarantined) fence_reuse(block)",
                      submitted)
        fence = compact(body(pool, "void ChunkGeometryPool::fence_reuse("))
        self.assertIn("glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)", fence)
        self.assertIn("block.reuse_quarantined = graphics_failed || !block.reuse_completion", fence)
        recycle = compact(body(pool, "void ChunkGeometryPool::recycle("))
        self.assertLess(recycle.index("--block.allocation_count == 0"),
                        recycle.index("block.reuse_quarantined = false"))
        release = compact(body(pool, "void ChunkGeometryPool::release_block("))
        self.assertIn("glDeleteSync(block.reuse_completion)", release)

    def test_pool_replacement_fence_failure_discards_only_unpublished_slice(self):
        pool = read(RENDERING / "passes/ChunkGeometryPool.cpp")
        update = compact(body(pool, "u32 ChunkGeometryPool::update("))
        self.assertIn("if (graphics_failed ||", update)
        failure = body(update, "if (graphics_failed ||")
        self.assertIn("m_allocations[replacement].live = false", failure)
        self.assertIn("recycle(replacement)", failure)
        self.assertNotIn("recycle(handle)", failure)
        self.assertNotIn("m_allocations[handle].live = false", failure)
        self.assertIn("return kInvalid;", failure)
        self.assertLess(update.index("pool_gl_error()"),
                        update.index("m_allocations[handle].live = false"))
        native = read(ROOT / "test/rendering/FarWorldWaterGl_test.cpp")
        for name in ("FullPoolLiveResidentReplacesRepeatedlyAfterFinalConsumerCompletion",
                     "FullPoolDualTailGrowthAndOversizedRefusalsAreTransactional",
                     "FullPoolMissingCompletionKeepsResidentDrawable",
                     "FullPoolFailedCompletionKeepsResidentDrawable",
                     "FullPoolFenceErrorKeepsResidentDrawable",
                     "FullPoolWaitErrorKeepsResidentDrawable",
                     "FullPoolCompletionCleanupErrorKeepsResidentDrawable",
                     "FullPoolRetirementAndResidentReuseShareEightPolls",
                     "ReplacementRetirementFailureKeepsBothResidentStoresAndVersion",
                     "HandleCapStillAdmitsCompletedResidentReplacement",
                     "WithinSlotBlockErrorsNeverFallThroughToResidentReuse",
                     "ExistingSliceInputAndGlErrorsNeverPublishReplacement"):
            self.assertIn("TEST_F(FarWorldWaterGl, " + name + ")", native)
        fill = body(native, "void Fill(u32 tail = 0)")
        self.assertIn("ASSERT_EQ(pool.storage_bytes(), ChunkGeometryPool::kMaxStorageBytes)", fill)
        self.assertIn("index_capacity * 2 - tail", fill)
        self.assertIn("PoolDrawableDigest(full.pool, handle)", native)
        self.assertIn("EXPECT_EQ(completion.polled, latest)", native)
        self.assertIn("EXPECT_EQ(completion.polls - polls, 8u)", native)

    def test_deferred_terrain_owns_only_the_valid_presented_resident(self):
        presented = compact(body(self.header, "void SetPresentedTerrainUpload("))
        self.assertIn("upload.snapshot_version = mesh_version;", presented)
        self.assertIn("upload.uploaded_version = mesh_version;", presented)
        self.assertIn("upload.uploaded_indices = element_count;", presented)
        self.assertIn("pool.valid_allocation(pool_handle, element_count)", presented)
        self.assertIn("data.SetPresentedTerrainUpload(upload, m_chunk_geometry_pool)", self.pipeline)
        water = compact(body(self.header, "void SetPresentedLadderWaterUpload("))
        self.assertIn("SetLadderWaterUpload(upload, mesh_version, enabled)", water)
        native = read(ROOT / "test/rendering/FarWorldWaterGl_test.cpp")
        self.assertIn("DeferredTerrainSnapshotKeepsPresentedOwnershipBeyondInnerSphere", native)
        self.assertIn("RetainedTerrainDrawSuppressesOverlappingFarPixelsDuringDeferral", native)
        self.assertIn("draw(resident.pool_handle)", native)
        self.assertIn("EXPECT_NEAR(Pixel(1).a * 255, presented ? 3 : 1, .01f)", native)
        self.assertIn("upload.snapshot_version = 8;", native)
        self.assertIn("EXPECT_EQ(upload.uploaded_version, 7u)", native)
        self.assertIn("EXPECT_EQ(ownership.Owns(overlap), refusal == 0)", native)

    def test_capture_writers_check_invalid_inputs_flush_close_and_partial_writes(self):
        pixels = compact(read(RENDERING / "PixelIo.cpp"))
        helpers = read(RENDERING / "PixelIo.h")
        image = compact(body(pixels, "bool WritePixelBufferPpm("))
        self.assertIn("!CapturePixelBytes(width, height, 3, bytes) || pixels.size() != bytes", image)
        self.assertIn("const bool written = WritePixelBufferPpmStream(output, width, height, pixels);", image)
        self.assertIn("const bool closed = CloseCaptureFile(output);", image)
        self.assertIn("if (!written || !closed)", image)
        flush = compact(body(helpers, "bool FlushCaptureStream("))
        self.assertEqual(flush, "output.flush(); return static_cast<bool>(output);")
        close = compact(body(helpers, "bool CloseCaptureFile("))
        self.assertIn("const bool flushed = FlushCaptureStream(output); output.close();", close)
        self.assertIn("return flushed && static_cast<bool>(output);", close)
        writer = compact(body(pixels, "bool WriteCaptureText("))
        self.assertIn("const bool written = WriteCaptureStream(output, text);", writer)
        self.assertIn("const bool closed = CloseCaptureFile(output);", writer)
        self.assertIn("return written && closed;", writer)
        stream = compact(body(helpers, "bool WriteCaptureStream("))
        self.assertLess(stream.index("output.write("), stream.index("return FlushCaptureStream(output)"))
        scan_source = read(RENDERING / "FrameScan.cpp")
        scan_read = compact(body(scan_source, "FrameScanReport ScanFrame("))
        self.assertIn("pipeline.internal_width() == static_cast<u32>(framebuffer_width)", scan_read)
        self.assertIn("pipeline.internal_height() == static_cast<u32>(framebuffer_height)", scan_read)
        self.assertIn("if (glGetError() != GL_NO_ERROR) return report;", scan_read)
        self.assertIn("glGetTextureSubImage(", scan_read)
        report = compact(body(scan_source, "bool WriteFrameScanReport("))
        self.assertIn("!report.ok || !CapturePixelBytes(", report)
        self.assertIn("report.total_pixels != bytes / 3", report)
        self.assertIn('return WriteCaptureText(out_path, j.dump(2) + "\\n");', report)
        native = read(ROOT / "test/rendering/frame_attribution_test.cpp")
        self.assertIn("TruncatedIndexFlushFailureAndCompleteStreamAreDistinct", native)
        gl_native = read(ROOT / "test/rendering/FarWorldWaterGl_test.cpp")
        self.assertIn("UnwritableReportImageInvalidScanAndCompleteOutput", gl_native)

    def test_legacy_water_migrates_at_equal_resolution(self):
        source = read(ROOT / "src/luminumbra_common/systems/WaterSystem.cpp")
        migration = compact(body(source, "std::size_t WaterSystem::MigrateChunksToSimResolution("))
        self.assertIn("== m_sim_resolution && HasCompleteIntegerWaterGrid(*cp, m_sim_resolution)",
                      migration)
        resize = compact(body(source, "bool WaterSystem::ResizeSimulationGrid("))
        self.assertIn("new_resolution == current_resolution && "
                      "!(chunk.water_bed_mm.empty() && chunk.water_depth_mm.empty())", resize)
        update = compact(body(source, "void WaterSystem::update("))
        self.assertIn("chunk_ptr->water_bed_mm.empty() && chunk_ptr->water_depth_mm.empty()",
                      update)
        native = read(ROOT / "test/common/WaterSeedInputs_test.cpp")
        for regression in ("SameResolutionLegacyFloatsMigrateAtLoadAndDirectResize",
                           "SameResolutionLegacyFloatsMigrateDuringLiveUpdate"):
            self.assertIn(regression, native)

    def test_legacy_surface_migration_does_not_require_flow(self):
        source = read(ROOT / "src/luminumbra_common/systems/WaterSystem.cpp")
        resize = compact(body(source, "bool WaterSystem::ResizeSimulationGrid("))
        self.assertIn("const bool can_interpolate_surface = old_water_levels.size() >= old_cell_count;",
                      resize)
        surface = compact(body(resize, "if (can_interpolate_surface)"))
        self.assertIn("chunk.water_level_data[idx] =", surface)
        self.assertNotIn("flow", surface)
        flow = compact(body(resize, "const auto flow_at ="))
        self.assertIn("index < old_flow_data.size() ? old_flow_data[index] : Vec2(0.0f)", flow)
        for corner in ("x0, z0", "x1, z0", "x0, z1", "x1, z1"):
            self.assertIn(f"flow_at({corner})", resize)
        native = read(ROOT / "test/common/WaterSeedInputs_test.cpp")
        self.assertIn("LegacySurfaceMigrationDefaultsOnlyMissingFlowSamples", native)
        self.assertIn("LegacyMissingOrShortSurfaceKeepsAvailableFlow", native)

    def test_water_migration_preserves_both_integer_grids_without_float_feedback(self):
        source = read(ROOT / "src/luminumbra_common/systems/WaterSystem.cpp")
        resize = compact(body(source, "bool WaterSystem::ResizeSimulationGrid("))
        self.assertIn("old_bed_mm = std::move(chunk.water_bed_mm)", resize)
        self.assertIn("sample_mm(old_bed_mm, x, z)", resize)
        self.assertIn("sample_mm(old_depth_mm, x, z)", resize)
        sampler = re.sub(r"//[^\n]*", "", body(body(source, "bool WaterSystem::ResizeSimulationGrid("), "const auto sample_mm ="))
        for forbidden in ("float", "floor", "lround"):
            self.assertNotIn(forbidden, sampler)
        self.assertIn("sum >= 0 ? sum / 65536 : -((-sum + 65535) / 65536)", sampler)
        self.assertIn("new_resolution == current_resolution", resize)
        self.assertIn("current_resolution > 32", resize)
        self.assertIn("chunk.water_bed_mm.empty() && chunk.water_depth_mm.empty()", resize)
        self.assertIn("chunk.water_bed_mm.size() == old_cell_count && "
                      "chunk.water_depth_mm.size() == old_cell_count", resize)
        refusal = "if (!legacy_float_only && !complete_integer_pair) { return false; }"
        self.assertIn(refusal, resize)
        self.assertLess(resize.index(refusal), resize.index("std::move(chunk.water_level_data)"))
        self.assertNotIn("can_interpolate_bed", resize)
        self.assertNotIn("can_interpolate_mm", resize)
        native = read(ROOT / "test/common/WaterSeedInputs_test.cpp")
        self.assertIn("CompleteIntegerPairStepsWithAbsentOrWrongSizedFlux", native)
        self.assertIn("LegacyFloatPairMigratesTogetherAndThenSteps", native)
        self.assertIn("EditedChannelAndDamSurviveMigrationAndSavedContinuation", native)
        self.assertIn("IntegerMigrationDoesNotRequireRenderMirrorsAndRefusesInvalidSourceResolution", native)
        self.assertIn("MigratedWaterOracle(beds, old_res, new_res)", native)
        self.assertIn("-4000, 8", native)
        self.assertIn("4000, 8", native)
        self.assertIn("step < 4", native)
        registration = read(ROOT / "test/CMakeLists.txt")
        self.assertIn("common/WaterSeedInputs_test.cpp", registration)
        self.assertIn("rendering/FarWorldWaterGl_test.cpp", registration)

    def test_rejected_water_grids_are_excluded_at_every_consumer(self):
        source = read(ROOT / "src/luminumbra_common/systems/WaterSystem.cpp")
        complete = body(source, 'bool HasCompleteIntegerWaterGrid(')
        for condition in ('resolution > 32', '32 % resolution',
                          'chunk.water_bed_mm.size() == cells',
                          'chunk.water_depth_mm.size() == cells'):
            self.assertIn(condition, complete)
        self.assertNotIn('water_edge_flux', complete)
        step = body(source, 'void StepChunkWaterFixed(')
        self.assertIn('c.water_edge_flux.assign(2 * static_cast<std::size_t>(n), 0)', step)
        self.assertLess(step.index('!HasCompleteIntegerWaterGrid(c, res)'),
                        step.index('c.water_edge_flux.assign('))
        self.assertLess(step.index('c.water_edge_flux.assign('),
                        step.index('flux[eidx]'))
        for signature in ('std::uint64_t WaterActivityScore(', 'bool HasCompleteWaterGrid(',
                          'void StepChunkWaterFixed(', 'void WaterSystem::update(',
                          'int WaterSystem::EditTerrainBed('):
            self.assertIn('HasCompleteIntegerWaterGrid(', body(source, signature))
        update = body(source, 'void WaterSystem::update(')
        self.assertIn('!HasCompleteIntegerWaterGrid(*chunk_ptr, GetWaterResolution(*chunk_ptr))', update)
        self.assertLess(update.index('!HasCompleteIntegerWaterGrid(*chunk_ptr'),
                        update.index('const bool sleeping_pre'))
        self.assertIn('!HasCompleteIntegerWaterGrid(*A, res)', update)
        self.assertIn('!HasCompleteIntegerWaterGrid(*B, res)', update)
        for signature in ('f32 WaterSystem::get_water_level_at(',
                          'Vec2 WaterSystem::get_water_flow_at(',
                          'void WaterSystem::apply_displacement('):
            self.assertIn('HasCompleteWaterGrid(', body(source, signature))
        native = read(ROOT / "test/common/WaterSeedInputs_test.cpp")
        for regression in ('RejectedAdjacentGridsNeverEnterSolverSeamsOrExternalEdits',
                           'InvalidResolutionAndMissingIntegerArraysCannotWakeOrStep',
                           'WorldWaterProbesRefuseUnsupportedResolutionBeforeSizeArithmetic'):
            self.assertIn('TEST(WaterSeedInputs, ' + regression + ')', native)
        world = read(ROOT / 'src/luminumbra_common/systems/SHIELD_WorldSystem.cpp')
        readiness = body(world, 'bool SHIELD_WorldSystem::debug_water_grid_at(')
        for condition in ('c->water_bed_mm.size() == cells',
                          'c->water_depth_mm.size() == cells',
                          'c->water_level_data.size() >= cells',
                          'c->water_flow_data.size() >= cells',
                          'c->water_sim_terrain_height.size() >= cells'):
            self.assertIn(condition, readiness)
        self.assertNotIn('water_edge_flux', readiness)
        self.assertIn('SourceReadinessRequiresTheSameArraysAsInjection', native)
        for signature in ('bool SHIELD_WorldSystem::IsUnderwater(',
                          'std::int64_t SHIELD_WorldSystem::debug_water_volume_near(',
                          'bool SHIELD_WorldSystem::debug_lowest_land_pos(',
                          'bool SHIELD_WorldSystem::debug_find_shoreline(',
                          'Vec3 SHIELD_WorldSystem::debug_deepest_water_pos(',
                          'float SHIELD_WorldSystem::live_water_surface_at(',
                          'bool SHIELD_WorldSystem::debug_water_grid_at('):
            self.assertIn('IsBoundedWaterResolution(res)', body(world, signature))

    def test_interactive_photos_publish_only_after_every_output_completed(self):
        pixels = read(RENDERING / "PixelIo.cpp")
        helpers = read(RENDERING / "PixelIo.h")
        capture = compact(body(pixels, "int PersistPhotoCapture("))
        files = compact(body(helpers, "bool WritePhotoCaptureFiles("))
        self.assertIn("const bool opened = image && thumbnail && sidecar;", files)
        self.assertIn("opened && WritePixelBufferPpmStream(image, width, height, pixels) && "
                      "WritePixelBufferThumbnailTgaStream(thumbnail, width, height, pixels, 512);", files)
        self.assertIn("payloads_written && image_closed && thumbnail_closed && "
                      "WriteCaptureStream(sidecar, metadata)", files)
        self.assertLess(files.index("const bool thumbnail_closed"), files.index("WriteCaptureStream(sidecar"))
        for signature, parameter in (("bool WritePixelBufferPpmStream(", "output"),
                                     ("bool WritePixelBufferThumbnailTgaStream(", "out")):
            serializer = compact(body(pixels, signature))
            self.assertIn("CapturePixelBytes(", serializer)
            self.assertIn("return FlushCaptureStream(" + parameter + ");", serializer)
        text = compact(body(helpers, "bool WriteCaptureStream("))
        self.assertIn("output.write(text.data(), static_cast<std::streamsize>(text.size()));", text)
        self.assertIn("return FlushCaptureStream(output);", text)
        for artifact in ("image", "thumbnail", "sidecar"):
            self.assertIn(f"const bool {artifact}_closed = CloseCaptureFile({artifact});", files)
        self.assertIn("return sidecar_written && sidecar_closed && publish();", files)
        for writer in ("WritePixelBufferPpmStream(image", "WritePixelBufferThumbnailTgaStream(thumbnail",
                       "WriteCaptureStream(sidecar, metadata)"):
            self.assertIn(writer, files)
        self.assertIn('return std::filesystem::create_directory(thumbnail_dir / "complete", error);', capture)
        self.assertIn("pixels, json, publish)) return 0;", capture)
        self.assertIn("EXPECT_FALSE(published)", read(ROOT / "test/rendering/FarWorldWaterGl_test.cpp"))
        self.assertIn('if (!std::filesystem::create_directory(thumbnail_dir, error)) return 0;', capture)
        self.assertEqual(capture.count("if (error) return 0;"), 3)
        self.assertIn("attempt < 1024", capture)
        self.assertNotIn("remove", capture.replace("No rollback deletes by pathname.", ""))
        for platform_flag in ("_O_EXCL", "O_EXCL", "O_NOFOLLOW"):
            self.assertIn(platform_flag, pixels)
        self.assertIn("O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW", pixels)
        self.assertIn("_O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY", pixels)
        self.assertIn("std::fclose(file) == 0", pixels)
        self.assertIn("std::fflush(m_file)", pixels)
        reader = compact(body(pixels, "bool ReadCompletedPhotoCapture("))
        self.assertIn('thumbnail_dir / "complete"', reader)
        for artifact in ('photo_dir / "image.ppm"', 'thumbnail_dir / "thumbnail.tga"',
                         'photo_dir / "verdict.photo.json"'):
            self.assertIn(artifact, reader)
        self.assertIn("sidecar.size() > buffer.size()", reader)
        self.assertIn("count != sidecar.size()", reader)
        self.assertIn("image.size() != expected_header.size() + bytes", reader)
        self.assertIn("thumbnail.size() != 18 + thumb_bytes", reader)
        overlay = compact(body(read(ROOT / "src/luminumbra_client/app/DebugOverlays.cpp"),
                               "void UpdatePhotoCapture("))
        self.assertIn("const bool readback_ok = ReadPhotoCapturePixels(cap_w, cap_h, px);", overlay)
        self.assertIn("readback_ok ? photo_allocator.Persist(", overlay)
        self.assertIn("if (saved_id > 0)", overlay)
        self.assertIn("Photo could not be saved.", overlay)
        gallery = compact(body(read(ROOT / "src/luminumbra_client/ui/Rml_UIManager.cpp"),
                               "void Rml_UIManager::PopulateGallery("))
        self.assertIn("!Rendering::ReadCompletedPhotoCapture(m_assetRoot, id, photo.stars, legacy)", gallery)
        self.assertIn('legacy ? ".tga" : "/thumbnail.tga"', gallery)
        self.assertIn("parsed.ptr != digits.data() + digits.size()", gallery)
        self.assertIn("if (ids.size() == 12) break;", gallery)
        self.assertIn("if (entries == kMaxGalleryEntries) { candidates.clear(); break; }", gallery)
        self.assertIn("if (ec) candidates.clear();", gallery)
        self.assertLess(gallery.index("std::sort(candidates.begin()"), gallery.index("ReadCompletedPhotoCapture("))
        self.assertIn("if (!ids.empty() && ids.back().id == photo.id) continue;", gallery)
        gallery_native = read(ROOT / "test/ui/ui_smoke_test.cpp")
        self.assertIn("kGalleryEntryBound = 65536", gallery_native)
        self.assertIn('WriteCaptureText(thumbs / "overflow", "unrelated")', gallery_native)
        wrapper = compact(body(read(ROOT / "src/luminumbra_client/app/WindowModeControls.cpp"),
                               "bool WriteCaptureThumbnailTga("))
        self.assertIn("return Luminumbra::Rendering::WritePixelBufferThumbnailTga(", wrapper)
        native = read(ROOT / "test/rendering/FarWorldWaterGl_test.cpp")
        for name in ("EveryPhotoArtifactOpenWriteFlushAndCloseFailureRefusesPublication",
                     "PhotoDirectoriesCompletionAndUnrelatedFilesArePreserved"):
            self.assertIn("TEST(CaptureArtifacts, " + name + ")", native)
        self.assertIn("artifact < 3", native)
        self.assertIn("failure < 4", native)

    def test_photo_allocation_covers_history_and_bounds_header_ranges(self):
        pixels = read(RENDERING / "PixelIo.cpp")
        occupied = body(pixels, "bool PhotoIdOccupied(")
        for reservation in ('photos / stamp', 'stamp + ".ppm"', 'stamp + ".photo.json"',
                            'thumbnails / cap', 'cap + ".tga"'):
            self.assertIn(reservation, occupied)
        self.assertIn('symlink_status(path, error)', occupied)
        initializer = body(pixels, "bool FindNextPhotoId(")
        self.assertIn('for (const bool thumbnail : {false, true})', initializer)
        self.assertIn('entries == kMaxReservationEntries', initializer)
        self.assertIn('if (error)', initializer)
        allocator = body(pixels, "int PhotoCaptureAllocator::Persist(")
        self.assertIn('!m_initialized || m_root != root', allocator)
        self.assertIn('FindNextPhotoId(root, next)', allocator)
        self.assertIn('saved + 1', allocator)
        overlay = body(read(ROOT / "src/luminumbra_client/app/DebugOverlays.cpp"),
                       "void UpdatePhotoCapture(")
        self.assertIn('photo_allocator.Persist(root_path_str, cap_w, cap_h, px, side)', compact(overlay))
        self.assertNotIn('root_path_str, app.hud.photoMode.captures', compact(overlay))
        reader = body(pixels, "bool ReadCompletedPhotoCapture(")
        self.assertIn('header.size() < 11', reader)
        self.assertLess(reader.index('header.size() < 11'), reader.index('std::from_chars('))
        self.assertIn('parsed_width.ptr == header.data() + header.size()', reader)
        native = read(ROOT / "test/rendering/FarWorldWaterGl_test.cpp")
        for regression in ('EveryLegacyAndDirectoryReservationBlocksIdReuse',
                           'AllocationCursorStartsBeyondHistoryAndSurvivesSessionRestarts',
                           'AllocationUsesEveryHistoricalFormatAndRefusesUnreadableRoots',
                           'EveryTruncatedHeaderPrefixRefusesBothCaptureFormats'):
            self.assertIn('TEST(CaptureArtifacts, ' + regression + ')', native)

    def test_gl_error_drains_exist_only_at_allowlisted_pre_work_sites(self):
        # Resource allocation/driver inspection has existing drains outside capture.
        # The only capture drain is immediately before interactive photo readback.
        drain = re.compile(
            r'while\s*\(\s*glGetError\(\)'
            r'|for\s*\([^;]*;[^;]*glGetError\(\)'
            r'|glGetError\(\)\s*==\s*GL_NO_ERROR\)\s*\{?\s*break;')
        infrastructure = {
            'src/luminumbra_client/core/HardwareInfo.cpp': 1,
            'src/luminumbra_client/rendering/RenderResourceRegistry.cpp': 2,
            'src/luminumbra_client/rendering/passes/ChunkGeometryPool.cpp': 2,
        }
        found = {}
        for path in sorted((ROOT / 'src').rglob('*')):
            if path.suffix not in ('.cpp', '.cc', '.h', '.hpp', '.inl', '.mm'):
                continue
            count = len(drain.findall(read(path)))
            if count:
                found[path.relative_to(ROOT).as_posix()] = count
        for path, count in infrastructure.items():
            self.assertEqual(found.pop(path, None), count)
        self.assertEqual(found, {'src/luminumbra_client/app/WindowModeControls.cpp': 1})
        readback = body(read(ROOT / 'src/luminumbra_client/app/WindowModeControls.cpp'),
                        'bool ReadPhotoCapturePixels(')
        self.assertEqual(readback.count('query < 8'), 2)
        self.assertEqual(readback.count('glGetError()'), 2)
        self.assertLess(readback.index('CapturePixelBytes('), readback.index('readback(bytes)'))
        self.assertLess(readback.index('++stale_errors'), readback.index('glReadBuffer(GL_BACK)'))
        self.assertIn('if (stale_errors != 0)', readback)
        self.assertEqual(readback.count('LUMINUMBRA_CORE_WARN('), 1)
        self.assertLess(readback.index('LUMINUMBRA_CORE_WARN('), readback.index('glReadBuffer(GL_BACK)'))
        self.assertLess(readback.index('glReadPixels('), readback.index('bool readback_ok = true'))
        self.assertIn('if (glGetError() != GL_NO_ERROR) {\n            readback_ok = false;', readback)
        self.assertIn('if (readback_ok)\n                pixels.swap(readback);', readback)
        self.assertIn('return readback_ok;', readback)
        native = read(ROOT / 'test/rendering/FarWorldWaterGl_test.cpp')
        for case in ('InteractivePhotoClearsStaleErrorsAndPublishesAllArtifacts',
                     'InteractivePhotoRefusesOwnReadbackErrorsWithoutPublishingOrDeleting'):
            self.assertIn('TEST_F(FarWorldWaterGl, ' + case + ')', native)

    def test_frame_errors_survive_every_post_render_capture(self):
        render = body(self.pipeline, 'void RenderPipeline::render_frame(')
        self.assertNotIn('glGetError()', render)
        scopes = [
            (RENDERING / 'FrameScan.cpp', 'FrameScanReport ScanFrame('),
            (RENDERING / 'SceneSurvey.cpp', 'bool RunSceneSurvey('),
            (RENDERING / 'TerrainCoverageCapture.cpp',
             'bool RenderPipeline::capture_terrain_coverage('),
            (RENDERING / 'static_preview/StaticRenderer.cpp',
             'StaticFrame StaticRenderer::Render('),
        ]
        for name in ('capture_frame_parity', 'capture_upscale_seam_parity',
                     'capture_ssao_parity'):
            scopes.append((RENDERING / 'RenderPipeline.cpp',
                           'bool RenderPipeline::' + name + '('))
        for path, signature in scopes:
            with self.subTest(path=path.name, signature=signature):
                scope = body(read(path), signature)
                queries = list(re.finditer(r'glGetError\(\)', scope))
                self.assertTrue(queries)
                for query in queries:
                    self.assertRegex(scope[query.end():], r'^\s*(?:==|!=)\s*GL_NO_ERROR')
                self.assertNotRegex(scope, r'glGetError\(\)[\s\S]{0,40}\bbreak;')
        native = read(ROOT / 'test/rendering/FarWorldWaterGl_test.cpp')
        regression = body(native, 'TEST_F(FarWorldWaterGl, FrameRenderErrorsStillRefuseEveryCapture)')
        self.assertIn('FrameRenderError failure(pipeline)', regression)
        self.assertIn('ASSERT_GT(failure.failures(), 0u)', regression)
        self.assertIn('EXPECT_FALSE(ScanFrame(pipeline, 9, 9, materials).ok)', regression)
        self.assertIn('EXPECT_FALSE(pipeline.capture_terrain_coverage(', regression)
        for name in ('capture_frame_parity', 'capture_upscale_seam_parity', 'capture_ssao_parity'):
            self.assertRegex(compact(regression), r'EXPECT_FALSE\(\s*pipeline\.' + name + r'\(')

    def test_survey_and_all_parity_siblings_propagate_required_artifact_failure(self):
        survey = compact(body(read(RENDERING / "SceneSurvey.cpp"), "bool RunSceneSurvey("))
        self.assertIn('if (!WriteCaptureText(out_dir / "survey.json",', survey)
        self.assertIn("if (!rep.ok) return false;", survey)
        self.assertIn('!WritePixelBufferPpm(out_dir / (t.label + ".ppm"), fbw, fbh, px)', survey)
        self.assertIn('!WriteFrameScanReport(rep, out_dir / (t.label + ".json"))) return false;', survey)
        self.assertIn('if (!WriteCaptureText(out_dir / "survey.json", index.dump(2) + "\\n"))', survey)
        self.assertLess(survey.index("!WriteFrameScanReport("), survey.index('index.push_back('))
        for kind in ("frame", "upscale_seam", "ssao"):
            capture = compact(body(self.pipeline, f"bool RenderPipeline::capture_{kind}_parity("))
            self.assertIn("CapturePixelBytes(w, h, 4, capture_bytes)", capture)
            self.assertLess(capture.index("m_screen_width > static_cast<u32>(std::numeric_limits<GLsizei>::max())"),
                            capture.index("const GLsizei w = static_cast<GLsizei>(m_screen_width)"))
            self.assertIn("CloseCaptureFile(", capture)
            self.assertIn("const bool report_written = CloseCaptureFile(", capture)
            self.assertIn("report_written && ok", capture)
        failure = compact(body(self.main, "const auto capture_failure ="))
        self.assertIn("exit_code = 2;", failure)
        self.assertIn('"capture_complete", false', failure)
        self.assertIn("!Luminumbra::Rendering::WriteCaptureText(", failure)
        self.assertIn("glfwSetWindowShouldClose(window, GLFW_TRUE)", failure)
        scan = compact(self.main.split("bool capture_ok = true;", 1)[1].split(
            "HandlePlayerVerbs(", 1)[0])
        self.assertIn('g_app.capture.render_parity_pass); capture_ok = false;', scan)
        self.assertIn("capture_ok = image_written && capture_ok", scan)
        self.assertIn("capture_ok = health_written && capture_ok", scan)
        self.assertIn("if (rep.ok)", scan)
        self.assertIn("if (capture_ok) capture_ok = Luminumbra::Rendering::WriteFrameScanReport(", scan)
        self.assertIn('if (!capture_ok) capture_failure(g_app.capture.frame_scan_path, "frame_capture_incomplete")', scan)
        self.assertLess(scan.index("capture_ok = image_written"), scan.index("WriteFrameScanReport("))

    def test_early_close_and_sibling_images_cannot_return_partial_capture_success(self):
        main = compact(self.main)
        self.assertIn("g_app.capture.frame_scan_active && !frame_scan_complete && exit_code == 0", main)
        self.assertIn("g_app.capture.survey_active && !survey_complete && exit_code == 0", main)
        self.assertIn("!scene_capture_complete && exit_code == 0", main)
        self.assertIn("frame_scan_complete = capture_ok;", main)
        self.assertIn("survey_complete = true;", main)
        self.assertIn("if (!scene_capture_complete) capture_failure(", main)
        self.assertIn("if (!screenshot_complete) capture_failure(", main)
        timelapse = compact(body(self.main, "static bool CaptureTimelapseFrame("))
        self.assertIn("CapturePixelBytes(vw, vh, 3, pixel_bytes)", timelapse)
        self.assertIn("!WritePixelBufferPpm(g_app.capture.timelapse_dir / nm, vw, vh, px)) return false;", timelapse)
        self.assertIn("if (!CaptureTimelapseFrame(", main)
        self.assertIn("g_app.capture.timelapse_captured < g_app.capture.timelapse_frames && exit_code == 0", main)


if __name__ == "__main__":
    unittest.main()
