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

    def test_retention_uses_pool_transaction_and_water_staging(self):
        pool = read(RENDERING / "passes/ChunkGeometryPool.cpp")
        update = body(pool, "u32 ChunkGeometryPool::update(")
        self.assertRegex(update, r"replace_upload\(\s*handle,\s*kInvalid,")
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
        allocation = body(pool, "u32 ChunkGeometryPool::allocate(")
        self.assertLess(allocation.index("block_index == kInvalid)\n            return kInvalid"),
                        allocation.index("std::memcpy("))

    def test_terrain_retirement_covers_every_submission_and_never_overwrites(self):
        pool = read(RENDERING / "passes/ChunkGeometryPool.cpp")
        pool_header = read(RENDERING / "passes/ChunkGeometryPool.h")
        update = body(pool, "u32 ChunkGeometryPool::update(")
        self.assertNotIn("std::memcpy", update)
        self.assertNotIn("return handle;", update)
        self.assertIn("return replace_upload(", update)
        release = compact(body(pool, "void ChunkGeometryPool::free("))
        self.assertIn("alloc.live = false;", release)
        self.assertEqual(release.count("recycle(handle)"), 1)
        self.assertLess(release.index("if (!m_blocks[alloc.block_index].submitted)"),
                        release.index("recycle(handle)"))
        self.assertIn("if (!m_blocks[alloc.block_index].submitted)", release)
        self.assertIn("m_retired.push_back({handle, glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)})", release)
        self.assertNotIn("free_vertex_slices", release)
        reclaim = compact(body(pool, "void ChunkGeometryPool::reclaim("))
        self.assertIn("std::min(kTerrainUploadLimits.count, m_retired.size())", reclaim)
        self.assertIn("count < visits", reclaim)
        self.assertIn("if (!retired.completion) { m_retired.push_back(retired); continue; }", reclaim)
        self.assertIn("glClientWaitSync(retired.completion, GL_SYNC_FLUSH_COMMANDS_BIT, 0)", reclaim)
        self.assertIn("if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) "
                      "{ m_retired.push_back(retired); continue; }", reclaim)
        self.assertLess(reclaim.index("GL_CONDITION_SATISFIED"), reclaim.index("recycle(retired.handle)"))
        self.assertIn("m_retired.pop_front()", reclaim)
        allocation = compact(body(pool, "u32 ChunkGeometryPool::allocate("))
        self.assertIn("reclaim();", allocation)
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
        self.assertLess(draw.index("glMultiDrawElementsIndirect("),
                        draw.index("m_chunk_geometry_pool.mark_submitted("))
        self.assertEqual(self.pipeline.count("glMultiDrawElementsIndirect("), 1)
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
        reclaim = compact(body(pool, "void ChunkGeometryPool::reclaim("))
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
        allocation = compact(body(pool, "u32 ChunkGeometryPool::allocate("))
        recycle = compact(body(pool, "void ChunkGeometryPool::recycle("))
        self.assertIn("++block.allocation_count;", allocation)
        self.assertIn("if (--block.allocation_count == 0) { block.vertex_high_water = 0; "
                      "block.index_high_water = 0; block.free_vertex_slices.clear(); "
                      "block.free_index_slices.clear(); } else {", recycle)
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
        self.assertIn("return CloseCaptureFile(output);", image)
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
