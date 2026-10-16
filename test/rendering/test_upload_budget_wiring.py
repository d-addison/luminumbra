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

    def test_both_live_backends_use_policy_and_include_preparation(self):
        self.assertIn("manage_mesh_gpu_resources<false>", self.pipeline)
        self.assertIn("manage_mesh_gpu_resources<true>", self.pipeline)
        integrate = body(self.pipeline, "void RenderPipeline::manage_mesh_gpu_resources(")
        self.assertLess(integrate.index("steady_clock::now()"), integrate.index("active.reserve"))
        self.assertLess(integrate.index("steady_clock::now()"), integrate.index("order_uploads("))
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
        self.assertIn("render_benchmark_frames + 1)", self.main)
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


if __name__ == "__main__":
    unittest.main()
