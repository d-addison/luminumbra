"""Check that presentation integration uses the tested clock-free policy."""
from pathlib import Path
import hashlib
import math
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

    def test_loading_allowance_is_explicit_and_shared_by_all_upload_lanes(self):
        preload = body(self.main, "auto wait_for_world_presentation =")
        self.assertIn("UploadPresentation::Loading", preload)
        # All entry implementations share this call; time-lapse and gameplay keep
        # the existing Streaming/Settled selection and its default arguments.
        self.assertEqual(self.main.count("UploadPresentation::Loading"), 1)
        prepare = compact(body(self.pipeline, "void RenderPipeline::prepare_frame("))
        self.assertIn("manage_chunk_gpu_resources(renderable_chunk_snapshots, camera, "
                      "snapshot_ms, presentation)", prepare)
        self.assertIn("manage_water_gpu_resources(renderable_chunk_snapshots, camera, presentation)",
                      prepare)
        manage = compact(body(self.pipeline, "void RenderPipeline::manage_mesh_gpu_resources("))
        self.assertIn("const auto limits = upload_limits(presentation, Water)", manage)
        self.assertIn("drain_uploads(queue, limits, cost, adopt, elapsed)", manage)
        self.assertIn("cost, adopt, elapsed, presentation)", manage)
        self.assertRegex(manage, r"m_near_terrain_refinement.prepare\(.*?elapsed\(\), presentation\)")
        policy = read(RENDERING / "NearTerrainRefinement.h")
        scheduler = compact(body(policy, "drain_terrain_uploads("))
        self.assertIn("upload_limits(presentation, false)", scheduler)
        self.assertIn("UploadBudget budget(limits)", scheduler)
        self.assertLess(scheduler.index("drain(baseline, false)"),
                        scheduler.index("drain(refinement, true)"))
        native = read(ROOT / "test/rendering/NearTerrainRefinement_test.cpp")
        for name in ("LoadingAllowanceDrainsBothLanesAndRefinementInBoundedIterations",
                     "LoadingAllowanceBoundsFailureRetriesAndHandlesEmptyQueues",
                     "LoadingAllowanceLeavesGameplayAndCaptureBudgetsUnchanged",
                     "LoadingRefinementPreservesPayloadAndRejectsInvalidElapsedTime"):
            self.assertIn("TEST(NearTerrainUpload, " + name + ")", native)
        self.assertIn("rendering/NearTerrainRefinement_test.cpp", read(ROOT / "test/CMakeLists.txt"))

    def test_loading_time_joins_frame_buckets_and_names_each_entry_stage(self):
        preload = body(self.main, "auto wait_for_world_presentation =")
        for field in ("total", "water", "render", "observe"):
            self.assertIn("entry_presentation_work." + field + ", true)", preload)
        self.assertIn("entry_presentation_work.scatter += scatter_ms", preload)
        self.assertIn("entry_presentation_work.foliage += foliage_ms", preload)
        self.assertIn("Attribution::phase_total_ms() - phases_before", preload)
        self.assertIn("Attribution::foliage_total_ms() - foliage_before", preload)
        loop = self.main.split("while (!glfwWindowShouldClose(window))", 1)[1]
        self.assertLess(loop.index("entry_presentation_work = {};"),
                        loop.index("const Attribution::FoliageFrameScope foliage_scope"))
        accounting = compact(loop).split("const double entry_stream_ms =", 1)[1].split(
            "const auto& foliage_frame =", 1)[0]
        self.assertIn("entry_presentation_work.water + entry_presentation_work.observe", accounting)
        self.assertIn("rb_stream_ms += entry_stream_ms", accounting)
        self.assertIn("rb_render_call_ms += entry_presentation_work.total - entry_stream_ms", accounting)
        self.assertIn("rb_render_bucket_ms += entry_presentation_work.total - entry_stream_ms - "
                      "entry_presentation_work.scatter - entry_presentation_work.nested_foliage - "
                      "entry_presentation_work.phases", accounting)
        self.assertIn("rb_scatter_ms += entry_presentation_work.scatter", accounting)
        self.assertIn("entry-split: total=", loop)
        self.assertIn("Entry readiness: stream_observations=", preload)
        self.assertIn("streaming.frames_observed", preload)
        self.assertIn("legacy_wanted=", preload)
        native = read(ROOT / "test/rendering/frame_attribution_test.cpp")
        self.assertIn("TEST(FrameAttribution, LoadingWorkSubtractsNestedCollectorsExactlyOnce)", native)

    def test_terrain_stream_reuse_requires_completion_or_fresh_storage(self):
        draw = body(self.pipeline, "void RenderPipeline::draw_chunks_mdi(")
        self.assertIn("TerrainStreamRing::kSlotCount> m_mdi_frames", self.header)
        ring = read(RENDERING / "TerrainStreamRing.h")
        self.assertIn("kSubmissionsPerFrame = 5", ring)
        self.assertIn("kFramesInFlight = 3", ring)
        self.assertIn("m_live[m_cursor] && !completion_signalled", ring)
        self.assertIn("m_mdi_stream.begin_frame();", body(self.pipeline, "void RenderPipeline::prepare_frame("))
        for empty in ("if (visible_chunks.empty() || block_count == 0)", "if (running == 0)"):
            self.assertIn("return;", body(draw, empty))
            self.assertLess(draw.index(empty), draw.index("m_mdi_stream.acquire("))
        # Only init/shutdown may clear lifetime state; replay restores a cursor.
        self.assertEqual(self.pipeline.count("m_mdi_stream = {};"), 2)
        self.assertEqual(draw.count("glClientWaitSync("), 1)
        self.assertIn("glClientWaitSync(frame.completion, 0, 0)", draw)
        self.assertIn("status == GL_ALREADY_SIGNALED || status == GL_CONDITION_SATISFIED", compact(draw))
        self.assertIn("bool completed = false;", draw)
        self.assertIn("m_mdi_stream.acquire( completed, diagnostic_replay || m_gpu_timers_suppressed || m_far_world_paired)", compact(draw))
        self.assertLess(draw.index("m_mdi_stream.acquire("), draw.index("ensure_mdi_capacity("))
        self.assertLess(draw.index("ensure_mdi_capacity(frame, running, allocation.orphan)"),
                        draw.index("glBufferSubData("))
        self.assertLess(draw.index("ReplayRollback retire_stream"), draw.index("const auto& blocks ="))
        retire = body(draw, "ReplayRollback retire_stream")
        self.assertIn("frame.completion = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)", retire)
        capacity = compact(body(self.pipeline, "void RenderPipeline::ensure_mdi_capacity("))
        pair_missing = "frame.indirect_buffer == 0 || frame.origin_buffer == 0"
        self.assertIn(pair_missing, capacity)
        self.assertIn("if (m_far_world_paired && (frame.command_capacity < running || " +
                      pair_missing + ")) throw", compact(draw))
        self.assertIn("if (!grow && !orphan) return;", capacity)
        self.assertEqual(capacity.count("glBufferData("), 2)
        self.assertNotIn("glDeleteBuffers", capacity)  # replay/VAO names remain valid
        self.assertLess(capacity.index("TerrainStreamRing::storage_bytes("), capacity.index("glGenBuffers("))
        destroy = body(self.pipeline, "void RenderPipeline::destroy_mdi_buffers()")
        self.assertIn("glDeleteSync(frame.completion)", destroy)
        for forbidden in ("glWaitSync(", "glFinish(", "GL_SYNC_FLUSH_COMMANDS_BIT"):
            self.assertNotIn(forbidden, draw + capacity)
        self.assertIn('"Terrain upload"', draw)
        self.assertIn("terrain_stream_diagnostics() const", self.header)
        regression = read(ROOT / "test/rendering/terrain_stream_ring_test.cpp")
        self.assertIn("FiveSubmissionsAcrossDelayedFramesNeverOverlapLiveStorage", regression)
        self.assertIn("ReplayRestoresSchedulingButNeverRollsBackLiveOwnership", regression)
        self.assertIn("rendering/terrain_stream_ring_test.cpp", read(ROOT / "test/CMakeLists.txt"))

    def test_terrain_stream_preserves_command_bytes_and_offset_zero_bindings(self):
        draw = body(self.pipeline, "void RenderPipeline::draw_chunks_mdi(")
        start = draw.index("std::vector<std::size_t> write_cursor = block_offset;")
        end = draw.index("out_draws = running;", start) + len("out_draws = running;")
        # Pin the original command/origin producer, independent of storage ownership.
        self.assertEqual(hashlib.sha256(compact(draw[start:end]).encode()).hexdigest(),
                         "f7c59add1a863ecb9ed1a98b43f6ac73e2e9aec529290c755a822987919490d1")
        for target, stride, data in (("GL_DRAW_INDIRECT_BUFFER", "DrawElementsIndirectCommand", "command"),
                                     ("GL_ARRAY_BUFFER", "glm::vec4", "origin")):
            self.assertIn(f"glBufferSubData({target}, 0, static_cast<GLsizeiptr>(running * sizeof({stride})), "
                          f"m_mdi_{data}_scratch.data())", compact(draw))
        self.assertIn("glBindVertexBuffer(1, frame.origin_buffer, 0, sizeof(glm::vec4))", compact(draw))

    def test_waterfall_sheets_retire_excavated_crest_and_landing_before_unknown_water(self):
        source = read(RENDERING / "WaterfallDetect.cpp")
        live = compact(body(source, "float LiveWaterFactorAt("))
        guard = "if (TerrainRemovesAnchor(world, anchor)) { return 0.0f; }"
        self.assertIn("if (site.terrain_anchors_known == 1)", live)
        self.assertIn("for (const auto& anchor : site.terrain_anchors)", live)
        self.assertIn(guard, live)
        self.assertLess(live.index(guard), live.index("debug_water_grid_at("))
        self.assertNotIn("GetTerrainHeightAt(", live[:live.index("debug_water_grid_at(")])
        detect = compact(body(source, "std::vector<WaterfallSite> DetectWaterfalls("))
        self.assertIn("site.terrain_anchors = std::array{", detect)
        self.assertIn("site.terrain_anchors_known = 1;", detect)
        for endpoint in ("crest", "foot"):
            self.assertIn("world.GetTerrainHeightAt(lip.%s.x, lip.%s.z) - 0.5f" % (endpoint, endpoint), detect)
        draw = compact(body(read(RENDERING / "passes/WaterfallPass.cpp"),
                            "void WaterfallPass::execute("))
        self.assertLess(draw.index("const float live = m_live_factors[i];"),
                        draw.index("if (live < 0.02f)"))
        update = compact(body(read(RENDERING / "passes/WaterfallPass.cpp"),
                              "void WaterfallPass::update_liveness("))
        self.assertIn("Rendering::LiveWaterFactorAt(world, m_sheet_sites[i])", update)
        self.assertLess(draw.index("continue;"), draw.index("glDrawArrays"))

    def test_waterfall_spray_reuses_prepared_site_liveness_before_particle_motion(self):
        source = read(RENDERING / "passes/WaterfallPass.cpp")
        update = compact(body(source, "void WaterfallPass::update_liveness("))
        self.assertEqual(source.count("Rendering::LiveWaterFactorAt("), 1)
        self.assertIn("for (std::size_t i = 0; i < m_sheet_sites.size(); ++i)", update)
        self.assertIn("m_live_factors[i] = std::isfinite(live) ? std::clamp(live, 0.0f, 1.0f) : 0.0f;", update)
        self.assertIn("particle_pass->set_waterfall_live_factors(m_live_factors);", update)
        for allocation in ("push_back", "resize", "reserve", "assign", "new "):
            self.assertNotIn(allocation, update)
        prepare = body(self.pipeline, "void RenderPipeline::prepare_frame(")
        self.assertLess(prepare.index("m_waterfall_pass->update_liveness(world_system, m_particle_pass.get());"),
                        prepare.index("m_particle_pass->update(deltaTime);"))
        adopt = compact(body(source, "bool WaterfallPass::adopt_next("))
        self.assertIn("const auto site_index = m_adopted_spray++;", adopt)
        self.assertIn("site.width, site_index);", adopt)
        particles = read(RENDERING / "passes/ParticlePass.cpp")
        add = compact(body(particles, "uint32_t ParticlePass::add_waterfall_spray("))
        self.assertIn("spray.waterfall_site = sheet_site;", add)
        self.assertIn("spray.spawn_scale = sheet_site == kInvalidEmitter ? 1.0f : 0.0f;", add)
        scale = compact(body(particles, "void ParticlePass::set_waterfall_live_factors("))
        self.assertIn("for (auto& emitter : m_active_emitters)", scale)
        self.assertIn("!emitter.waterfall_spray || emitter.waterfall_site == kInvalidEmitter", scale)
        self.assertIn("emitter.waterfall_site < live_factors.size() ? live_factors[emitter.waterfall_site] : 0.0f", scale)
        self.assertIn("emitter.spawn_scale = std::isfinite(live) ? std::clamp(live, 0.0f, 1.0f) : 0.0f;", scale)
        self.assertNotIn("spawn_rate =", scale)
        self.assertNotIn("m_particles", scale)
        spawn = compact(body(particles, "void ParticlePass::spawn_from_emitter("))
        self.assertIn("emitter.data.spawn_rate * emitter.spawn_scale", spawn)

    def test_waterfall_roar_consumes_live_selection_gain_and_repositions_on_handoff(self):
        audio = read(ROOT / "src/luminumbra_client/app/FrameAudio.cpp")
        living = compact(body(audio, "void UpdateLivingWorldAudio("))
        self.assertIn("const auto source = renderPipeline.waterfall_roar_source(pc);", living)
        self.assertIn("const auto* best = source.site;", living)
        self.assertIn('SetAmbientVolume("waterfall_roar", source.gain(roar.volume));', living)
        self.assertIn("if (!roarOn || roarPosition != best->crest)", living)
        handoff = compact(body(living, "if (!roarOn || roarPosition != best->crest)"))
        self.assertIn('PlayAmbientLoop( "waterfall_roar", best->crest, 400.0f);', handoff)
        self.assertIn("roarPosition = best->crest;", handoff)
        self.assertIn('else if (roarOn) { audioManager->StopAmbientLoop("waterfall_roar"); roarOn = false; }', living)
        self.assertNotIn("waterfall_sites(", living)
        self.assertNotIn("LiveWaterFactorAt(", audio)
        forward = compact(body(self.header, "WaterfallPass::RoarSource waterfall_roar_source("))
        self.assertEqual(forward, "return m_waterfall_pass->roar_source(listener);")
        header = read(RENDERING / "passes/WaterfallPass.h")
        self.assertIn("return SelectRoarSource(m_sheet_sites, m_live_factors, listener);", header)
        self.assertIn("return base_gain * live_factor;", header)
        select = compact(body(read(RENDERING / "passes/WaterfallPass.cpp"),
                              "WaterfallPass::RoarSource WaterfallPass::SelectRoarSource("))
        self.assertIn("if (!std::isfinite(live) || live <= 0.0f) { continue; }", select)
        self.assertIn("std::min(sites.size(), live_factors.size())", select)
        self.assertIn(".live_factor = std::min(live, 1.0f)", select)
        self.assertNotIn("world", select.replace("no world queries", ""))

    def test_static_prop_spatial_query_precedes_instance_work_and_retains_streams(self):
        source = read(RENDERING / "passes/GBufferPass.cpp")
        draw = body(source, "void GBufferPass::geometry_pass_static_meshes(")
        collect = body(draw, "for (const std::size_t propIndex : candidates)")
        self.assertLess(draw.index("static_prop_candidates("), draw.index("glm::length("))
        query = body(source, "GBufferPass::static_prop_candidates(")
        self.assertIn("m_staticPropIndex.Query(", query)
        self.assertIn("BuildStaticPropSpatialIndex(", query)
        self.assertIn("const CachedStaticProp& cp = m_staticPropCache[propIndex];", collect)
        self.assertIn("const bool authoredTree = cp.authoredTree;", collect)
        self.assertNotIn("starts_with", collect)
        self.assertNotIn("kv.second.mats.clear()", draw)
        self.assertNotIn("kv.second.tints.clear()", draw)
        mesh_append = body(collect, "if (batch.selection.Select(propIndex))")
        self.assertIn("batch.mats.push_back(cp.model);", mesh_append)
        self.assertIn("batch.tints.push_back(cp.tint);", mesh_append)
        atlas_append = body(collect, "if (m_impostorSelection.Select(propIndex))")
        self.assertIn("impostorInstances.push_back({cp.model, glm::vec4(cp.tint, 1.0f)});", atlas_append)
        self.assertIn("kv.second.selection.Begin();", draw)
        self.assertIn("kv.second.selection.Finish()", draw)
        self.assertIn("m_impostorSelection.Begin();", draw)
        self.assertIn("m_impostorSelection.Finish()", draw)
        cache = body(source, "void GBufferPass::build_static_prop_cache(")
        self.assertIn('cp.authoredTree = bp.starts_with("game-assets/tree-small-02/");', cache)
        self.assertIn("m_staticPropIndexValid = false;", cache)
        bounds = body(source, "StaticPropSpatialIndex BuildStaticPropSpatialIndex(")
        self.assertIn("item.lodBounds[3].Include(item.lodBounds[2]);", bounds)
        self.assertIn("impostorRadius * cp.maxScale", bounds)
        replay = body(source, "if (replay)")
        self.assertIn("std::swap(m_impostorSelection, impostor_selection);", replay)
        header = read(RENDERING / "passes/GBufferPass.h")
        self.assertIn("return m_staticPropIndex.work();", header)
        cmake = read(ROOT / "test/CMakeLists.txt")
        self.assertIn("rendering/StaticPropSpatialIndex_test.cpp", cmake)

    def test_visible_rocks_and_shrubs_check_resident_support_before_batching(self):
        source = read(RENDERING / "passes/GBufferPass.cpp")
        cache = compact(body(source, "void GBufferPass::build_static_prop_cache("))
        self.assertIn('cp.terrainAnchored = bp.starts_with("procgen://rock_") || '
                      'bp.starts_with("procgen://bush_");', cache)
        draw = compact(body(source, "void GBufferPass::geometry_pass_static_meshes("))
        self.assertIn("cp.anchorSlot = slot->second;", cache)
        # Each anchor chunk resolves once per frame; edited support drops the prop.
        lookup = "if (cp.anchorSlot < m_anchorChunks.size() && ctx.world_system) {"
        resolve = "auto chunk = ctx.world_system->find_streamed_chunk(slot.coords); if (chunk && chunk->has_terrain_authority()) {"
        guard = "if (cp.supportRemoved) { continue; }"
        # Support is re-evaluated only when the slot's chunk, revision or SDF storage changes.
        changed = ("if (!same_owner || slot.supportRevision != revision || slot.supportSdf != sdf) {")
        reeval = ("if (cp.supportGeneration != slot.supportGeneration) { cp.supportRemoved = "
                  "EditedTerrainRemovesAnchor(slot.chunk.get(), cp.position); "
                  "cp.supportGeneration = slot.supportGeneration; }")
        for needle in (lookup, "if (!slot.resolved) { slot.resolved = true;", resolve, guard,
                       "const auto revision = slot.chunk->voxel_revision();",
                       "const float* const sdf = slot.chunk->sdf_data.data();",
                       "!slot.supportOwner.owner_before(slot.chunk) && !slot.chunk.owner_before(slot.supportOwner);",
                       changed, reeval):
            self.assertIn(needle, draw)
        self.assertLess(draw.index(resolve), draw.index(changed))
        self.assertLess(draw.index(changed), draw.index(reeval))
        self.assertLess(draw.index(reeval), draw.index(guard))
        self.assertEqual(draw.count("EditedTerrainRemovesAnchor("), 1)
        header = compact(read(RENDERING / "passes/GBufferPass.h"))
        for field in ("mutable std::uint32_t supportGeneration = 0; mutable bool supportRemoved = false;",
                      "std::weak_ptr<const ::Luminumbra::Chunk> supportOwner;",
                      "std::uint32_t supportGeneration = 1;"):
            self.assertIn(field, header)
        self.assertLess(draw.index("if (culled)"), draw.index(lookup))
        self.assertLess(draw.index(lookup), draw.index(guard))
        self.assertLess(draw.index(guard), draw.index("batch.mats.push_back(cp.model)"))
        support = compact(read(RENDERING / "TerrainAnchor.h"))
        for check in ("!chunk->has_terrain_authority()", "chunk->sdf_data.size() != volume",
                      "!std::isfinite(sample)", "density > 0.5f",
                      "std::floor(static_cast<double>(position[axis]) / CHUNK_SIZE_X)"):
            self.assertIn(check, support)
        self.assertNotIn("get_renderable_chunks", support)
        self.assertNotIn("get_density_at(", support)

    def test_renderable_owner_accepts_independent_complete_water_geometry(self):
        world = read(ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        selection = body(world, "SHIELD_WorldSystem::renderable_chunks_view()")
        selection = body(selection, "for (const auto& [id, chunk] : m_streaming_state.chunks)")
        predicate = re.search(r"if \((.*?)\) \{\s*m_renderable_chunks.push_back", selection,
                              re.S).group(1)
        geometry = body(world, "bool HasRenderableGeometry(")
        geometry = re.search(r"return (.*?);", geometry, re.S).group(1)
        predicate = predicate.replace("HasRenderableGeometry(*chunk)", "(" + geometry + ")")
        predicate = predicate.replace("chunk->get_state() == ChunkState::Ready", "ready")
        for field in ("mesh_vertices", "mesh_indices", "water_mesh_vertices", "water_mesh_indices"):
            predicate = re.sub(r"chunk(?:->|\.)" + field + r"\.empty\(\)", field, predicate)
        predicate = compact(predicate).replace("&&", " and ").replace("||", " or ")
        predicate = re.sub(r"!(?!=)", " not ", predicate).strip()
        # Check water-only first, then all ready/empty/partial/complete combinations.
        for bits in (25, *range(32)):
            ready, terrain_vertices, terrain_indices, water_vertices, water_indices = (
                bool(bits & (1 << bit)) for bit in range(5))
            actual = eval(predicate, {"__builtins__": {}}, dict(
                ready=ready, mesh_vertices=not terrain_vertices, mesh_indices=not terrain_indices,
                water_mesh_vertices=not water_vertices, water_mesh_indices=not water_indices))
            expected = ready and ((terrain_vertices and terrain_indices) or
                                  (water_vertices and water_indices))
            self.assertEqual(actual, expected, bits)

    def test_water_geometry_changes_invalidate_selection_at_every_publication(self):
        world = read(ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        publication = body(world, "bool SHIELD_WorldSystem::publish_front_meshing_batch(")
        water = body(publication, "if (replaces_water && water_current &&")
        transition = body(water, "if (chunk->water_mesh_vertices.empty() !=")
        self.assertEqual(compact(transition), "notify_renderable_chunks_changed();")
        self.assertIn("chunk->pending_water_mesh_vertices.empty() || "
                      "chunk->water_mesh_indices.empty() != "
                      "chunk->pending_water_mesh_indices.empty()", compact(water))
        self.assertLess(water.index("notify_renderable_chunks_changed();"),
                        water.index("chunk->water_mesh_vertices ="))
        scheduler = body(world, "const auto collect_candidate =")
        departure = body(scheduler, "if (!water_lod_requested && required_lod >= 0 &&")
        self.assertIn("ClearPublishedWaterMesh(*this, *chunk_ptr);", departure)
        removal = body(world, "void ClearPublishedWaterMesh(")
        self.assertLess(removal.index("notify_renderable_chunks_changed();"),
                        removal.index("chunk.water_mesh_vertices.clear();"))
        self.assertIn("ClearPublishedWaterMesh(*this, *chunk);", world)
        mesher = read(ROOT / "src/luminumbra_common/world/MarchingCubes.cpp")
        direct = body(mesher, "void GenerateWaterMesh(")
        ready = body(direct,
                     "if (chunk.get_state() == ChunkState::Ready && has_water() != had_water)")
        self.assertIn("world_system.notify_renderable_chunks_changed();", ready)
        self.assertEqual(direct.count("notify_if_presence_changed();"), 2)
        self.assertLess(direct.index("const bool had_water = has_water();"),
                        direct.index("chunk.water_mesh_vertices.clear();"))
        self.assertLess(direct.index("notify_renderable_chunks_changed();"),
                        direct.index("const int resolution"))

    def test_terrain_hierarchy_and_cache_exclude_water_only_owners(self):
        hierarchy = read(RENDERING / "passes/HierarchicalCuller.cpp")
        build = body(hierarchy, "void RenderPipeline::HierarchicalCuller::BuildHierarchy(")
        loop = body(build, "for (const auto& chunk : chunks)")
        self.assertEqual(compact(body(loop, "if (!chunk.has_terrain_mesh())")), "continue;")
        self.assertLess(loop.index("if (!chunk.has_terrain_mesh())"),
                        loop.index("chunk_refs.push_back("))
        self.assertEqual(compact(body(build, "if (chunk_refs.empty())")),
                         "m_root.reset(); return;")
        cache = body(self.pipeline, "void RenderPipeline::ensure_terrain_culling_hierarchy(")
        ids = body(cache, "for (const auto& chunk : renderable_chunks)")
        self.assertEqual(compact(body(ids, "if (chunk.has_terrain_mesh())")),
                         "sorted_chunk_ids.push_back(chunk.id);")
        self.assertIn("m_terrainCullingCache.chunk_count != sorted_chunk_ids.size()", cache)
        self.assertIn("m_terrainCullingCache.chunk_count = sorted_chunk_ids.size();", cache)
        prepare = body(self.pipeline, "void RenderPipeline::prepare_frame(")
        self.assertIn("if (chunk.has_terrain_mesh() && found != m_chunk_render_data.end())", prepare)
        water = body(self.pipeline, "void RenderPipeline::execute_stage_opaque_snapshot(")
        self.assertIn("for (const auto& chunk : renderable_chunk_snapshots)", water)
        self.assertIn("m_frame_water_input.draw_items.push_back(", water)

    def test_water_only_dispatch_preserves_ready_without_terrain_work(self):
        world = read(ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        dispatch = body(world, "SHIELD_WorldSystem::dispatch_meshing_jobs(")
        condition = re.search(r"if \(([^\n]+)\) \{\s*"
                              r"set_renderable_chunk_state\(\*chunk, "
                              r"Luminumbra::ChunkState::Meshing\);", dispatch).group(1)
        condition = condition.replace("&&", " and ").replace("!", " not ").strip()
        for required in (False, True):
            for active in (False, True):
                actual = eval(condition, {"__builtins__": {}},
                              dict(terrain_mesh_required=required, has_active_mesh=active))
                self.assertEqual(actual, required and not active)

    def test_water_only_activity_retires_terrain_allocation_and_ground_support(self):
        manage = body(self.pipeline, "void RenderPipeline::manage_mesh_gpu_resources(")
        activity = compact(body(manage, "for (const auto& chunk : chunks)"))
        self.assertEqual(activity.count("active.insert(chunk.id);"), 1)
        # Evaluate the actual admission condition for both template instantiations.
        # The old unconditional insert must fail on a water-only terrain record.
        match = re.search(r"if \((.*?)\) \{ active.insert\(chunk.id\); \}", activity)
        condition = match.group(1) if match else "True"
        condition = condition.replace("chunk.has_terrain_mesh()", "terrain")
        condition = condition.replace("||", " or ").replace("&&", " and ")
        for water in (False, True):
            for terrain in (False, True):
                actual = eval(condition, {"__builtins__": {}}, dict(Water=water, terrain=terrain))
                self.assertEqual(actual, water or terrain, (water, terrain))
        # Retain the existing grace periods and shared resource teardown.
        self.assertIn("constexpr u32 inactive_ttl = Water ? 30 : 15;", manage)
        inactive = body(manage, "for (auto& [id, data] : records)")
        self.assertIn("if (active.contains(id))", inactive)
        self.assertIn("if (++data.frames_since_inactive > inactive_ttl)", inactive)
        self.assertIn("expired.push_back(id);", inactive)
        teardown = body(manage, "for (const auto id : expired)")
        self.assertIn("unload_chunk_resources(id);", teardown)
        self.assertIn("unload_water_resources(id);", teardown)
        unload = body(self.pipeline, "void RenderPipeline::unload_chunk_resources(")
        for statement in ("m_foliage_pass->remove_ground_mesh(chunk_id);",
                          "m_chunk_geometry_pool.free(data.pool_handle);",
                          "m_chunk_render_data.erase(it);"):
            self.assertIn(statement, unload)

    def test_column_water_inputs_are_shared_before_owner_filtering(self):
        world = read(ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        source = compact(body(world, "SHIELD_WorldSystem::column_water_mesh_source("))
        self.assertIn("column->second.water_layers", source)
        self.assertIn("resident->second.get() != &chunk", source)
        self.assertIn("resolution > 1 && resolution <= 32", source)
        self.assertIn("return *layer;", source)
        self.assertIn("water_layers.find(0)", source)
        self.assertIn("complete(*zero->second)", source)
        dispatch = compact(body(world, "SHIELD_WorldSystem::dispatch_meshing_jobs("))
        self.assertIn("const auto& water_source = column_water_mesh_source(*chunk);", dispatch)
        self.assertIn("snapshot.water_mesh_inputs_captured = true;", dispatch)
        for channel in ("water_level_data", "water_flow_data", "water_sim_terrain_height"):
            self.assertIn("copy_mirror(water_source." + channel + ", snapshot." + channel + ");",
                          dispatch)
        self.assertIn("snapshot.water_depth_mm = water_source.water_depth_mm;", dispatch)
        self.assertIn("MeshingSnapshotBytes(*chunk, terrain_mesh_required, water_source)", dispatch)
        mesher = compact(body(read(ROOT / "src/luminumbra_common/world/MarchingCubes.cpp"),
                              "void GenerateWaterMesh("))
        self.assertIn("chunk.water_mesh_inputs_captured ? chunk : "
                      "world_system.column_water_mesh_source(chunk)", mesher)
        self.assertIn("const float surface = water_source.water_level_data[cell_index];", mesher)
        self.assertIn("HasCompleteWaterGrid(water_source, resolution)", mesher)
        self.assertEqual(mesher.count("SampleChunkWaterLevel(water_source,"), 4)
        self.assertIn("water_source.water_depth_mm[", mesher)
        self.assertNotIn("SampleChunkWaterLevel(chunk,", mesher)

    def test_column_owner_filter_partitions_boundaries_and_rejects_nonfinite_samples(self):
        mesher = body(read(ROOT / "src/luminumbra_common/world/MarchingCubes.cpp"),
                      "void GenerateWaterMesh(")
        predicate = re.search(r"if \((!std::isfinite\(surface\).*?)\) \{", mesher,
                              re.S).group(1)
        predicate = compact(predicate).replace("std::isfinite", "math.isfinite")
        predicate = predicate.replace("chunk.water_mesh_min_y", "lower")
        predicate = predicate.replace("chunk.water_mesh_max_y", "upper")
        predicate = predicate.replace("!", " not ").replace("||", " or ").strip()
        bounds = (-math.inf, -48.0, 16.0, math.inf)
        for surface in (-1000.0, -48.001, -48.0, 0.0, 15.999, 16.0, 17.0, 1000.0,
                        math.nan, math.inf, -math.inf):
            owners = sum(not eval(predicate, {"__builtins__": {}, "math": math},
                                  dict(surface=surface, lower=lower, upper=upper))
                         for lower, upper in zip(bounds, bounds[1:]))
            self.assertEqual(owners, int(math.isfinite(surface)), surface)
        for corner in ("water_h00", "water_h10", "water_h01", "water_h11"):
            self.assertIn("!std::isfinite(" + corner + ")", mesher)

    def test_column_residency_and_simulation_wakes_reach_all_owners(self):
        world = read(ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        for mutation in re.finditer(r"m_activation_index\.(insert|erase)\([^;]+;", world):
            self.assertRegex(world[mutation.end():], r"^\s*invalidate_column_water_mesh\(")
        wake = compact(body(world, "SHIELD_WorldSystem::invalidate_column_water_mesh("))
        self.assertIn("column->second.water_layers", wake)
        self.assertIn("layer->water_mesh_generated.store(false, std::memory_order_release);", wake)
        self.assertIn("m_water_system->retain_mesh_invalidation(layer->get_id());", wake)
        self.assertIn("++m_water_column_mesh_work.owner_visits;", wake)
        water = read(ROOT / "src/luminumbra_common/systems/WaterSystem.cpp")
        invalidate = body(water, "void InvalidateWaterMesh(")
        self.assertIn("++chunk.water_mesh_invalidation_version;", invalidate)
        self.assertIn("InvalidateWaterMesh(*to_init[i], m_mesh_invalidations);", water)
        cadence = body(water, "bool WaterKernels::AdvanceWaterMeshCadence(")
        self.assertIn("++chunk.water_mesh_invalidation_version;", cadence)
        propagation = compact(body(world, "SHIELD_WorldSystem::propagate_column_water_mesh_invalidations("))
        self.assertIn("m_water_system->mesh_invalidations()", propagation)
        retirement = body(world, "if (resident == m_streaming_state.chunks.end() ||")
        self.assertIn("ClearUnavailableWaterMesh(*this,", retirement)
        self.assertIn("ClearUnavailableWaterMesh(*this, resident, m_streaming_state.chunks.end());",
                      compact(retirement))
        clear = body(world, "void ClearUnavailableWaterMesh(")
        self.assertIn("if (resident == end)", clear)
        self.assertIn("Chunk& owner = *resident->second;", clear)
        self.assertIn("ClearPublishedWaterMesh(world_system, owner);", clear)
        # Clearing published geometry publishes the empty replacement's version.
        self.assertLess(clear.index("const bool had_geometry"),
                        clear.index("ClearPublishedWaterMesh(world_system, owner);"))
        self.assertIn("if (had_geometry) {", compact(clear))
        self.assertIn("owner.water_mesh_version.fetch_add(1);", clear)
        self.assertIn("++owner.water_mesh_owner_version;", clear)
        self.assertIn("for (const auto id : invalidations)", propagation)
        self.assertIn("++m_water_column_mesh_work.pending_visits;", propagation)
        self.assertIn("columns.insert(key).second", propagation)
        self.assertLess(propagation.index("columns.insert(key).second"),
                        propagation.index("column_water_mesh_source("))
        self.assertIn("column->second.water_mesh_invalidation_version == "
                      "selected.water_mesh_invalidation_version", propagation)
        self.assertIn("column->second.water_mesh_source_id == selected.get_id()", propagation)
        self.assertLess(propagation.index("for (auto* source : sources)"),
                        propagation.index("invalidate_column_water_mesh(*source);"))
        self.assertLess(wake.index("m_water_system->retain_mesh_invalidation(layer->get_id());"),
                        wake.index("m_activation_index.acknowledge_water_mesh_invalidation(source);"))
        self.assertIn("propagate_column_water_mesh_invalidations();", body(compact(world), "void SHIELD_WorldSystem::update(entt::registry& registry, const std::vector<Vec3>&"))
        regional = read(ROOT / "src/luminumbra_common/systems/RegionalWater.cpp")
        self.assertIn("copy.water_mesh_invalidation_version = original.water_mesh_invalidation_version;",
                      body(regional, "void CaptureWater("))
        publish = body(regional, "void PublishWater(")
        self.assertIn("destination.water_mesh_invalidation_version = staged.water_mesh_invalidation_version;", publish)
        self.assertIn("++grid.water_mesh_invalidation_version;", body(regional, "void Transaction::PrepareGrid("))
        index = read(ROOT / "src/luminumbra_common/world/ActivationResidencyIndex.h")
        self.assertIn("update_water_owners(column);", body(index, "void insert("))
        self.assertIn("update_water_owners(it->second);", body(index, "void erase("))
        update = compact(body(index, "void update_water_owners("))
        self.assertIn("++m_counters.water_owner_visits;", update)
        self.assertIn("++chunk.water_mesh_owner_version;", update)
        self.assertNotIn("m_columns", update)

    def test_column_regressions_are_registered(self):
        native = read(ROOT / "test/shield/async_water_meshing_test.cpp")
        for name in ("DeepSeabedResidentLayersKeepSeaLevelSurface",
                     "ResidentOwnershipCoversBelowAboveAndGaps",
                     "SurfaceOwnerArrivalSupersedesPendingWaterAndRemeshesColumn",
                     "StackedWaterOnlyChunksEmitOnlyContainingSurface",
                     "StackedWaterSurfaceBoundaryHasExactlyOneOwner",
                     "StackedSlopingWaterAssignsEachCellIndependently",
                     "OwnerIntervalsInvalidateOnlyTheAffectedResidentStack",
                     "DivergedLayersUseOneColumnSampleAcrossTheBoundary",
                     "ColumnSourceRejectsUnsupportedPartialAndUninitializedGrids",
                     "NonFiniteColumnSamplesNeverPublishNonFiniteVertices",
                     "PendingColumnSnapshotRejectsAChangedSourceWithUnchangedBounds",
                     "ColumnSourcePrefersTheSimulationSourceLayerOverLowerResidents",
                     "LegacyAndRegionalCadenceWakeRemoteOwnersOnceAndRefusalDoesNot",
                     "DisabledColumnSourceClearsRemoteOwnerAndRejectsPendingWater"):
            self.assertIn("TEST(AsyncWaterMeshing, " + name + ")", native)
        self.assertIn("shield/async_water_meshing_test.cpp", read(ROOT / "test/CMakeLists.txt"))
        self.assertIn("test/rendering/test_upload_budget_wiring.py",
                      read(ROOT / "tools/ci/ci_paths.py"))

    def test_stale_column_snapshots_cannot_publish_water(self):
        world = read(ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        publication = compact(body(world, "bool SHIELD_WorldSystem::publish_front_meshing_batch("))
        for field in ("water_mesh_min_y", "water_mesh_max_y", "water_mesh_owner_version"):
            self.assertIn("chunk->" + field + " == job_chunk.scratch->" + field, publication)
        self.assertIn("column_water_mesh_source(*chunk).get_id() == "
                      "job_chunk.scratch->water_mesh_source_id", publication)
        self.assertIn("const bool replaces_water = water_owner_current &&", publication)
        self.assertIn("const auto& water_source = column_water_mesh_source(*chunk);", publication)
        self.assertIn("water_source.water_level_data.begin()", publication)

    def test_water_only_regressions_use_registered_targets(self):
        cmake = read(ROOT / "test/CMakeLists.txt")
        for source in ("rendering/render_smoke_test.cpp", "shield/async_water_meshing_test.cpp",
                       "common/DeterministicWorkBudget_test.cpp"):
            self.assertIn(source, cmake)
        self.assertIn('add_test(NAME UploadBudgetWiringPython', cmake)
        self.assertIn('"test/rendering/test_upload_budget_wiring.py"',
                      read(ROOT / "tools/ci/ci_paths.py"))
        smoke = body(read(ROOT / "test/rendering/render_smoke_test.cpp"),
                     "TEST(RenderSmokeTest, WaterOnlyOwnerSubmitsWaterAndTracksTerrainPresence)")
        for assertion in ("terrain_gpu_chunks, 0u", "terrain_vertex_capacity, 0u",
                          "terrain_index_capacity, 0u", "water_gpu_chunks, 1u",
                          "EXPECT_TRUE(foliage.instances().empty())"):
            self.assertIn(assertion, smoke)
        async_tests = read(ROOT / "test/shield/async_water_meshing_test.cpp")
        for name in ("WaterOnlySelectionTracksPublicationInBothDirections",
                     "DryWaterOnlyDispatchPreservesReadyUntilDuePublication",
                     "HeightmapWaterDepartureInvalidatesReadyWaterOnlySelection",
                     "DirectWaterSelectionInvalidatesForDryAndInvalidGrids"):
            self.assertIn("TEST(AsyncWaterMeshing, " + name + ")", async_tests)

    def test_world_presentation_blocks_both_entry_paths_and_timelapse(self):
        preload = body(self.main, "auto wait_for_world_presentation =")
        self.assertNotIn("world->update(", preload)
        self.assertIn("PrepareInitialWaterPresentationNear(", preload)
        self.assertNotIn("TickSimulation(", preload)
        self.assertIn("ObserveWorldPresentation(", preload)
        self.assertIn("return entry_presentation_gate.waiting();", preload)
        staged = body(self.main, "auto run_entry_stage =")
        activation = staged.split("case WorldEntryStage::Activate:", 1)[1].split(
            "case WorldEntryStage::Count:", 1)[0]
        self.assertRegex(compact(activation), r"if \(wait_for_world_presentation\(\)\) \{ "
                         r"return WorldEntryResult::Pending; \} activate_loaded_world\(\);")
        loading = self.main.split("case GameState::WORLD_LOADING:", 1)[1].split(
            "case GameState::IN_GAME:", 1)[0]
        self.assertIn("const bool waiting = wait_for_world_presentation();", loading)
        self.assertRegex(compact(loading), r"if \(waiting\) \{ .*?DrawLoadingOverlay\(.*?"
                         r"\} else \{ activate_loaded_world\(\);")
        bypass = body(self.main, "if (bypass_loading_ui)")
        self.assertNotIn("GameState::IN_GAME", bypass)
        self.assertNotIn("kTimelapseSettleFrames", self.main)
        self.assertIn("const bool simulate_frame = !timelapse_waiting &&", compact(self.main))
        self.assertIn("scenario_drive = scenario_runner && !timelapse_waiting ?", compact(self.main))
        for hook in ("onPreRenderWorldSweep();", "onPostRenderCapture(deltaTime);"):
            self.assertRegex(compact(self.main), r"if \(scenario_runner && !timelapse_waiting\) \{ "
                             + re.escape("scenario_runner->" + hook))
        capture = body(self.main, "static bool CaptureTimelapseFrame(")
        self.assertIn("if (!presentation_ready)", capture)
        observation = body(self.main, "ObserveWorldPresentation(")
        self.assertNotIn("coverage.near_field_renderable", observation)
        for signal in ("NearTerrainPrepared(coverage.expected_surface_chunks", "coverage.ready_surface_chunks",
                       "coverage.pending_lod_chunks", "diagnostic.missing == 0",
                       "diagnostic.stale == 0", "diagnostic.pending == 0",
                       "far_world.coverage_settled", "InitialWaterPrepared(water_prepared,",
                       "uploads.water_uploads_deferred",
                       "foliage->build_pending()", "pipeline.render_uploads_settled()",
                       "streaming.quiescent()"):
            self.assertIn(signal, observation)

    def test_initial_water_requires_preparation_and_automated_readiness_survives_timeout(self):
        policy = read(ROOT / "src/luminumbra_client/world/WorldPresentation.h")
        water = body(policy, "constexpr bool InitialWaterPrepared(")
        self.assertEqual(compact(water), "return prepared && deferred_uploads == 0;")
        decision = body(policy, "constexpr bool permits_entry(")
        self.assertEqual(compact(decision),
                         "return !waiting() && (!automated || (!timed_out() && runtime_ready));")
        activation = body(self.main, "auto activate_loaded_world =")
        self.assertIn("EvaluateReadiness(scenario_config, gameSession.get())", activation)
        self.assertIn("if (!entry_presentation_gate.permits_entry(true, last_readiness_report.ready))",
                      activation)
        self.assertNotIn("entry_presentation_gate.timed_out()", activation)
        self.assertIn("scenario_ready = false;", activation)
        self.assertIn("scenario_ready = false;", body(self.main, "auto finish_world_entry ="))
        preload = body(self.main, "auto wait_for_world_presentation =")
        self.assertLess(preload.index("PrepareInitialWaterPresentationNear("),
                        preload.index("renderPipeline.render_frame("))
        common = read(ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        prepare = body(common, "bool SHIELD_WorldSystem::PrepareInitialWaterPresentationNear(")
        self.assertNotIn("update_chunk_activation(", prepare)
        self.assertNotIn("TickSimulation(", prepare)
        self.assertIn("InitializeGeneratedWater(presentation)", prepare)
        self.assertIn("GenerateWaterMesh(*m_water_system, *this, presentation)", prepare)
        self.assertIn("chunk->water_mesh_generated.store(true", prepare)

    def test_preview_restore_keeps_guard_and_precedes_every_foliage_consumer(self):
        restore = body(self.main, "auto restore_worldgen_preview =")
        self.assertIn("currentState != GameState::MAIN_MENU || !g_uiManager ||", restore)
        self.assertIn("!g_uiManager->GetWorldCreationPreviewState().active", restore)
        self.assertIn("renderPipeline.apply_graphics(resolved_graphics)", restore)
        self.assertIn("worldgenPreview->set_active(false)", restore)
        self.assertLess(self.main.index("if (worldgenPreview->active() &&"),
                        self.main.index("UpdateFoliageScatter(g_app,"))
        preload = body(self.main, "auto wait_for_world_presentation =")
        self.assertLess(preload.index("restore_worldgen_preview()"),
                        preload.index("UpdateFoliageScatter(g_app,"))
        self.assertLess(self.main.index("restore_worldgen_preview();",
                                       self.main.index("while (!glfwWindowShouldClose")),
                        self.main.index("RenderMenuScreens(g_app,"))

    def test_workload_load_finishes_presentation_before_residency_requires_in_game(self):
        load = body(self.main, "[&](bool reload)")
        self.assertLess(load.index("start_world_load(id)"),
                        load.index("wait_for_world_presentation()"))
        self.assertLess(load.index("wait_for_world_presentation()"),
                        load.index("activate_loaded_world()"))
        self.assertLess(load.index("activate_loaded_world()"),
                        load.index("reload_settling_pending ="))
        self.assertIn("pass < Luminumbra::Client::kWorldPresentationFrameDeadline", load)
        self.assertIn("gameStateManager.GetCurrentState() == GameState::WORLD_LOADING", load)
        self.assertIn("gameStateManager.GetCurrentState() != GameState::IN_GAME", load)
        self.assertIn("!scenario_ready", load)
        self.assertIn("scenario_failed = true;", load)
        self.assertIn("!glfwWindowShouldClose(window)", load)
        self.assertIn("benchmark_queries->checkpoint_passes()", load)
        self.assertLess(load.index("benchmark_queries->checkpoint_passes()"),
                        load.index("activate_loaded_world()"))
        self.assertLess(load.index("if (glfwWindowShouldClose(window))"),
                        load.index("activate_loaded_world()"))
        self.assertIn("glfwPollEvents()", load)
        self.assertIn("MainLoopHeartbeat().fetch_add", load)

    def test_timelapse_mode_and_final_camera_settle_before_first_capture(self):
        setup = self.main.index("const bool mode_ready =")
        self.assertLess(self.main.index("ws->debug_find_shoreline("), setup)
        self.assertLess(self.main.index("ws->debug_lowest_land_pos("), setup)
        reset = self.main.index("timelapse_presentation_gate.restart_for_view();", setup)
        self.assertLess(setup, reset)
        self.assertNotIn("timelapse_presentation_started_at = {};", self.main[setup:reset + 100])
        water = self.main.index("const bool water_prepared =", reset)
        render = self.main.index("renderPipeline.render_frame(", water)
        observe = self.main.index("timelapse_presentation_gate.observe(", render)
        allow = self.main.index("timelapse_presentation_ready =", observe)
        self.assertLess(reset, render)
        self.assertLess(render, observe)
        self.assertLess(observe, allow)
        self.assertIn("timelapse_mode_prepared &&", self.main[allow:allow + 250])
        self.assertIn("timelapse_mode_preparation_failed", self.main)
        self.assertIn("timelapse_presentation_timeout", self.main)
        self.assertLess(self.main.index("PrepareInitialWaterPresentationNear(", reset), render)
        self.assertIn("timelapse_mode_prepared || !timelapse_presentation_gate.waiting()",
                      compact(self.main))

    def test_near_refinement_runs_only_in_admitted_terrain_uploads(self):
        manage = body(self.pipeline, "void RenderPipeline::manage_mesh_gpu_resources(")
        drain = manage[manage.index("const auto adopt ="):]
        self.assertIn("copy_terrain_mesh_payload(candidate.chunk, payload)", compact(drain))
        self.assertLess(drain.index("if (!copied)"),
                        drain.index("m_near_terrain_refinement.prepare("))
        self.assertLess(drain.index("m_near_terrain_refinement.prepare("),
                        drain.index("upload_chunk_mesh(candidate.chunk, payload)"))
        water = body(drain, "if constexpr (Water)")
        self.assertNotIn("m_near_terrain_refinement", water)
        self.assertIn("payload.vertices, payload.indices, elapsed()", compact(drain))
        self.assertIn("NearTerrainUploadResult::Deferred", drain)
        self.assertIn("payload.refinement.set(result == NearTerrainUploadResult::Deferred ? nullptr", compact(drain))

    def test_live_refinement_copies_exact_owner_lattice_after_admission_and_validation(self):
        policy = read(RENDERING / "NearTerrainRefinement.h")
        prepare = body(policy, "prepare(")
        self.assertIn("chunk.sdf_data.size() == World::kNearTerrainDensityCount", prepare)
        self.assertIn("const std::vector<float> density_copy", prepare)
        self.assertIn("? chunk.sdf_data", prepare)
        self.assertNotIn("std::move(chunk.sdf_data)", prepare)
        self.assertIn("std::span<const float>(density_copy)", prepare)
        self.assertLess(prepare.index("observe(chunk, version, evidence, distance_squared)"),
                        prepare.index("const std::vector<float> density_copy"))
        self.assertLess(prepare.index("elapsed_ms >= kTerrainUploadLimits.milliseconds"),
                        prepare.index("const std::vector<float> density_copy"))
        self.assertLess(prepare.index("const std::vector<float> density_copy"),
                        prepare.index("RefineNearTerrain("))
        native = read(ROOT / "test/rendering/NearTerrainRefinement_test.cpp")
        for regression in ("LiveEligibilityCopiesItsLatticeWithoutMovingAuthority",
                           "LiveLatticeRefusesMissingPartialOversizedAndInvalidSamples"):
            self.assertIn("TEST(NearTerrainUpload, " + regression + ")", native)

    def test_saturated_deadline_preserves_baselines_and_separately_schedules_refinement(self):
        manage = body(self.pipeline, "void RenderPipeline::manage_mesh_gpu_resources(")
        self.assertIn("const auto adopt =", manage)
        adopt = body(manage, "const auto adopt =")
        deferred = body(adopt, "if (candidate.refinement_only && result == NearTerrainUploadResult::Deferred)")
        self.assertEqual(compact(deferred), "return false;")
        self.assertIn("payload.refinement.set(result == NearTerrainUploadResult::Deferred ? nullptr",
                      compact(adopt))
        self.assertLess(adopt.index("payload.refinement.set("),
                        adopt.index("upload_chunk_mesh(candidate.chunk, payload)"))
        self.assertIn("auto& destination = refinement_only ? refinement_candidates : candidates", manage)
        self.assertIn("drain_terrain_uploads( queue, m_near_terrain_upload_queue, cost, adopt, elapsed, presentation)",
                      compact(manage))
        policy = read(RENDERING / "NearTerrainRefinement.h")
        scheduler = body(policy, "drain_terrain_uploads(")
        self.assertEqual(scheduler.count("UploadBudget budget("), 1)
        self.assertLess(scheduler.index("drain(baseline, false)"),
                        scheduler.index("drain(refinement, true)"))
        for guard in ("!std::isfinite(elapsed_ms)", "elapsed_ms < 0",
                      "elapsed_ms >= kTerrainUploadLimits.milliseconds",
                      "bytes > kTerrainUploadLimits.bytes - used.bytes"):
            self.assertIn(guard, scheduler)
        prepare = body(policy, "prepare(")
        self.assertLess(prepare.index("!m_entries[found->second].refined"),
                        prepare.index("elapsed_ms >= kTerrainUploadLimits.milliseconds"))
        for signature in ("void RenderPipeline::clear_all_chunk_data()",
                          "void RenderPipeline::cleanup_gpu_resources()"):
            self.assertIn("m_near_terrain_upload_queue.clear()", body(self.pipeline, signature))
        native = read(ROOT / "test/rendering/NearTerrainRefinement_test.cpp")
        for regression in ("SaturatedDeadlineStillUploadsBaselineMeshes",
                           "CachedRefusalsRemainImmediateAfterTheDeadline",
                           "OptionalReplacementsShareLimitsAndNeverClaimProgressAllowance",
                           "OptionalFailureRotatesWithoutRepeatingAndCanResume",
                           "DeadlineAfterOptionalCopyPreservesResidentAndRetries"):
            self.assertIn("TEST(NearTerrainUpload, " + regression + ")", native)

    def test_near_refinement_cost_reserves_expansion_before_payload_copy(self):
        manage = body(self.pipeline, "void RenderPipeline::manage_mesh_gpu_resources(")
        self.assertLess(manage.index("NearTerrainRefinementCache::upload_cost("),
                        manage.index("const auto adopt ="))
        policy = read(RENDERING / "NearTerrainRefinement.h")
        cost = body(policy, "static std::size_t upload_cost(")
        self.assertIn("vertices > kNearTerrainMaxInputVertices", cost)
        self.assertIn("indices / 3 > kNearTerrainMaxInputTriangles", cost)
        self.assertIn("indices % 3 != 0", cost)
        self.assertIn("mesh_upload_bytes(", cost)
        self.assertIn("original_bytes + refined_bytes", cost)
        prepare = body(policy, "prepare(")
        self.assertLess(prepare.index("elapsed_ms >= kTerrainUploadLimits.milliseconds"),
                        prepare.index("RefineNearTerrain("))
        self.assertIn("NearTerrainUploadResult::Deferred", prepare)

    def test_near_refinement_cache_keys_bounds_and_lifecycle(self):
        policy = read(RENDERING / "NearTerrainRefinement.h")
        self.assertIn("kCapacity = 64", policy)
        self.assertIn("std::array<Entry, kCapacity>", policy)
        observe = body(policy, "observe(")
        self.assertIn("evidence->eligible(chunk, version, distance_squared)", compact(observe))
        self.assertIn("entry.version != version || entry.evidence != current", observe)
        prepare = body(policy, "prepare(")
        self.assertLess(prepare.index("m_index.find(chunk.get_id())"),
                        prepare.index("RefineNearTerrain("))
        self.assertIn("forget(*m_entries[m_next].id)", prepare)
        self.assertLess(prepare.index("m_index.emplace("),
                        prepare.index("forget(*m_entries[m_next].id)"))
        for signature in ("void RenderPipeline::clear_all_chunk_data()",
                          "void RenderPipeline::cleanup_gpu_resources()"):
            self.assertIn("m_near_terrain_refinement.clear()", body(self.pipeline, signature))
        self.assertIn("m_near_terrain_refinement.forget(chunk_id)",
                      body(self.pipeline, "void RenderPipeline::unload_chunk_resources("))

    def test_near_refinement_resident_stamp_detects_evidence_and_range_changes(self):
        manage = body(self.pipeline, "void RenderPipeline::manage_mesh_gpu_resources(")
        self.assertLess(manage.index("m_near_terrain_refinement.observe("),
                        manage.index("if (!is_new && !is_stale)"))
        self.assertIn("!resident->second.refinement.matches(evidence)", manage)
        self.assertIn("is_stale = is_stale || refinement_changed", manage)
        upload = body(self.pipeline, "void RenderPipeline::upload_chunk_mesh(")
        self.assertLess(upload.index("if (new_handle == ChunkRenderData::kInvalidPoolHandle)"),
                        upload.index("render_data.refinement = payload.refinement"))
        self.assertIn("payload.vertices, payload.indices", upload)

    def test_same_version_republished_refinement_uses_required_queue(self):
        manage = body(self.pipeline, "void RenderPipeline::manage_mesh_gpu_resources(")
        # A resident request includes both refined output and cached refusals.
        # Changed eligible evidence must retire either through required work;
        # the first upgrade from an unrefined baseline remains optional.
        self.assertIn("retire_refined = found != records.end() && found->second.refinement.requested",
                      compact(manage))
        classification = re.search(r"const bool refinement_only\s*=\s*(.*?);", manage, re.S)
        self.assertIsNotNone(classification)
        self.assertEqual(compact(classification.group(1)),
                         "!Water && !is_new && !is_stale && refinement_requested && !retire_refined")
        self.assertLess(manage.index("is_stale = is_stale || refinement_changed"),
                        manage.index("if (!is_new && !is_stale)"))
        self.assertIn("auto& destination = refinement_only ? refinement_candidates : candidates",
                      manage)
        self.assertIn("refresh_uploads( queue, candidates,", compact(manage))
        self.assertIn("drain_terrain_uploads( queue, m_near_terrain_upload_queue, cost, adopt, elapsed, presentation)",
                      compact(manage))

    def test_last_deferred_baseline_enqueues_refinement_before_settled_accounting(self):
        manage = body(self.pipeline, "void RenderPipeline::manage_mesh_gpu_resources(")
        adopt = body(manage, "const auto adopt =")
        terrain = body(adopt, "else {")
        guard = ("if (result == NearTerrainUploadResult::Deferred && "
                 "failures == stats.terrain_upload_failures)")
        self.assertIn(guard, compact(terrain))
        enqueue = body(terrain, "if (result == NearTerrainUploadResult::Deferred &&")
        for statement in ("auto refinement = candidate;", "refinement.is_new = false;",
                          "refinement.is_stale = true;", "refinement.refinement_only = true;",
                          "m_near_terrain_upload_queue.refresh(candidate.chunk.id, refinement);"):
            self.assertIn(statement, enqueue)
        # Failed copies/uploads retain required work; optional deferrals retain
        # their queue entry; completed/refused requests never enqueue a retry.
        self.assertLess(adopt.index("if (!copied)"), adopt.index(guard.split(" &&")[0]))
        self.assertLess(terrain.index("if (candidate.refinement_only &&"), terrain.index("auto refinement ="))
        self.assertLess(terrain.index("upload_chunk_mesh(candidate.chunk, payload)"),
                        terrain.index("auto refinement ="))
        accounting = "queue.size() + (Water ? 0 : m_near_terrain_upload_queue.size()) + invalid_payloads"
        self.assertIn(accounting, compact(manage))
        self.assertLess(manage.index("m_near_terrain_upload_queue.refresh(candidate.chunk.id, refinement)"),
                        manage.index(accounting))
        settled = body(self.header, "bool render_uploads_settled() const")
        self.assertIn("m_last_mesh_upload_stats.terrain_uploads_deferred == 0", settled)

    def test_near_refinement_retired_or_emptied_mesh_releases_displaced_surface(self):
        manage = body(self.pipeline, "void RenderPipeline::manage_mesh_gpu_resources(")
        residents = body(manage, "for (auto& [id, data] : records)")
        self.assertIn("m_near_terrain_refinement.forget(id)", residents)
        self.assertIn("found->second.refinement.requested", manage)
        self.assertIn("(!Water && !retire_refined)", manage)
        drain = manage[manage.index("const auto adopt ="):]
        self.assertIn("unload_chunk_resources(candidate.chunk.id)", drain)
        copy = body(self.pipeline, "bool RenderPipeline::copy_terrain_mesh_payload(")
        self.assertIn("(chunk.terrain_vertex_count == 0) != (chunk.terrain_index_count == 0)", copy)
        self.assertIn("payload.vertices.empty() != payload.indices.empty()", copy)
        self.assertNotIn("!chunk.has_terrain_mesh()", copy)

    def test_near_refinement_setting_is_off_persistent_and_applied_transactionally(self):
        presets = ROOT / "src/luminumbra_common/core"
        self.assertIn("bool terrain_near_refinement = false;", read(presets / "GraphicsPresets.h"))
        source = read(presets / "GraphicsPresets.cpp")
        self.assertIn(".terrain_near_refinement = false,", source)
        self.assertIn('key == "terrain_near_refinement"', source)
        self.assertIn("values.terrain_near_refinement = *enabled", source)
        self.assertIn("values.terrain_near_refinement);", source)
        apply = body(self.pipeline, "bool RenderPipeline::apply_graphics(")
        self.assertLess(apply.index("if (!resize_graphics_targets("),
                        apply.index("m_near_terrain_refinement.set_enabled("))
        self.assertIn("resolved.values.terrain_near_refinement", apply)
        native = read(ROOT / "test/rendering/NearTerrainRefinement_test.cpp")
        self.assertIn("SettingDefaultsAndPersistenceRoundTrip", native)
        self.assertIn("config.SaveUserOverlay(file.Path().string())", native)
        self.assertIn('SystemConfig::LoadLayered("", file.Path().string())', native)

    def test_near_refinement_ui_toggle_uses_boolean_graphics_listing(self):
        import xml.etree.ElementTree as ET
        markup = ET.parse(ROOT / "data/ui/settings.rml").getroot()
        controls = {item.get("id"): item for item in markup.iter() if item.get("id")}
        control = controls["graphics_terrain_near_refinement"]
        self.assertEqual(control.get("data-key"), "terrain_near_refinement")
        self.assertIn("graphics-control", control.get("class"))
        self.assertEqual([option.get("value") for option in control], ["on", "off"])
        self.assertIn("graphics_terrain_near_refinement_session", controls)
        ui = read(ROOT / "src/luminumbra_client/ui/Rml_UIManager.cpp")
        self.assertIn('edit.key == "grass_enabled" || edit.key == "terrain_near_refinement"', ui)
        self.assertIn('document->GetElementById("graphics_terrain_near_refinement")', ui)
        self.assertIn('values.terrain_near_refinement ? "on" : "off"', ui)

    def test_far_render_integration_uses_region_freshness_and_keeps_strict_default(self):
        legacy = read(RENDERING / "FarLodSystem.cpp")
        current = body(legacy, "bool FarLodSystem::build_result_current(")
        self.assertIn("is_far_lod_sdf_snapshot_current(*result.sdf_snapshot, true)", current)
        self.assertIn("far_lod_region_authority_revision(result.rx, result.rz)", current)
        world_header = read(ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.h")
        self.assertIn("bool region_scoped = false", world_header)
        world = read(ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        validation = body(world, "bool SHIELD_WorldSystem::is_far_lod_sdf_snapshot_current(")
        regional = body(validation, "if (region_scoped)")
        self.assertIn("snapshot.region_authority_revision", regional)
        self.assertIn("far_lod_region_authority_revision(", regional)
        self.assertNotIn("m_far_lod_authority_revision.load", regional)
        self.assertIn("} else if (snapshot.authority_revision !=", validation)
        for retained in ("m_durable_read_failed", "snapshot.params_hash",
                         "HasPublishedFarLodSdf", "published_sdfs == captured_resident_sdfs"):
            self.assertIn(retained, validation)

    def test_ladder_preparation_refusal_advances_legacy_in_the_same_frame(self):
        selection = read(RENDERING / "FarWorldSelection.h")
        dispatch = body(selection, "void AdvanceSelectedFarWorld(")
        fallback = dispatch[dispatch.index("ladder();") + len("ladder();"):]
        self.assertIn("selection.advance", fallback)
        self.assertIn("selection.effective == FarWorldMode::Legacy", fallback)
        self.assertIn("legacy();", fallback)
        frame = body(self.pipeline, "void RenderPipeline::prepare_frame(")
        advance = frame[frame.index("AdvanceSelectedFarWorld("):]
        self.assertLess(advance.index("m_far_world->PrepareFrame("),
                        advance.index("select_frame_far_world("))
        tests = read(ROOT / "test/rendering/FarWorldLegacyIsolation_test.cpp")
        for case in ("PreparationRefusalAdvancesAndSubmitsLegacyInTheSameFrame",
                     "PreparationRefusalHonorsSuppressionAndDoesNotRetry",
                     "SuccessfulPreparationDoesNotAdvanceLegacy"):
            self.assertIn(case, tests)

    def test_impostor_collection_requires_submitted_resident_terrain_at_each_tree(self):
        gbuffer = read(RENDERING / "passes/GBufferPass.cpp")
        collect = body(gbuffer, "for (const std::size_t propIndex : candidates)")
        self.assertIn("if (impostorsOn && authoredTree && lod == 3 && cp.impostorTree) {", collect)
        self.assertIn("impostor_candidate = true;", collect)
        self.assertIn("input.submitted_live_terrain->covers(cp.position)", collect)
        self.assertNotIn("kFarClipInnerRadiusMeters", collect)
        self.assertNotIn("live_terrain_owns_at", collect)
        self.assertIn("impostor_candidate && !live_terrain_owned &&", collect)
        self.assertIn("input.legacy_far->terrain_resident_at(cp.position)", collect)
        decision = ("SelectStaticPropSubmission( input.far_selection, impostor_candidate, "
                    "live_terrain_owned, far_terrain_resident)")
        self.assertIn(decision, compact(collect))
        guard = "if (submission == StaticPropSubmission::Refused)"
        fallback = body(collect, guard)
        self.assertIn("lod = 2;", fallback)
        self.assertNotIn("continue;", fallback)
        self.assertNotIn("return", fallback)
        tree = body(collect, "if (submission == StaticPropSubmission::Impostor)")
        self.assertIn("impostorInstances.push_back", tree)
        self.assertTrue(compact(tree).endswith("continue; // all LOD3 tree parts are folded into the billboard"))
        self.assertLess(collect.index(decision.split("(")[0]), collect.index(guard))
        self.assertLess(collect.index(guard), collect.index("impostorInstances.push_back"))
        self.assertLess(collect.index("impostorInstances.push_back"), collect.index("const auto rit = resolve_static_prop_lod(ctx, input, cp, lod)"))
        legacy = read(RENDERING / "FarLodSystem.cpp")
        policy = compact(body(legacy, "StaticPropSubmission SelectStaticPropSubmission("))
        self.assertLess(policy.index("if (!impostor_candidate)"), policy.index("if (live_terrain_owned)"))
        self.assertLess(policy.index("if (live_terrain_owned)"), policy.index("if (!selection.submit"))
        self.assertEqual(compact(body(policy, "if (live_terrain_owned)")),
                         "return StaticPropSubmission::Impostor;")
        self.assertTrue(policy.endswith("return StaticPropSubmission::Impostor;"))
        self.assertIn("if (!selection.submit || !far_terrain_resident)", policy)
        query = body(legacy, "bool FarLodSystem::terrain_resident_at(")
        self.assertEqual(query.count("m_residents.find("), 1)
        self.assertNotIn("for (", query)
        for retained in ("!m_enabled", "std::isfinite", "m_outer_clip_m",
                         "inner_clipped", "m_preview_mode", "m_preview_inner_radius",
                         "found->second.element_count != 0"):
            self.assertIn(retained, query)

    def test_refused_impostors_resolve_farthest_available_mesh_through_ordinary_batch(self):
        collect = body(read(RENDERING / "passes/GBufferPass.cpp"),
                       "for (const std::size_t propIndex : candidates)")
        resolver = body(read(RENDERING / "passes/GBufferPass.cpp"), "GBufferPass::resolve_static_prop_lod(")
        memo = body(resolver, "if (rit == m_resolveMemo.end())")
        self.assertIn("resolve_mesh(candidatePath, candidatePath)", memo)
        self.assertIn("for (int mesh_lod = lod - 1; !resolved.first && mesh_lod >= 0; --mesh_lod)", memo)
        self.assertIn("resolve_mesh(fallbackPath, fallbackPath)", memo)
        self.assertIn("LodMeshPath(basePath, mesh_lod)", memo)
        self.assertIn("if (cp.authoredTree)", memo)
        self.assertIn("m_resolveMemo", memo)
        for retained in ("cp.model * glm::vec4(glm::vec3(mesh->boundingSphere), 1.0f)",
                         "batch.materialId = cp.materialId", "batch.mats.push_back(cp.model)",
                         "batch.tints.push_back(cp.tint)"):
            self.assertIn(retained, collect)
        tests = read(ROOT / "test/rendering/render_smoke_test.cpp")
        regression = body(tests, "TEST(RenderSmokeTest, RefusedTreeImpostorsSubmitAvailableMeshLods)")
        for required in ("FarWorldMode::Legacy", "FarWorldMode::Ladder", "highest_lod",
                         "capture.mesh_draws", "capture.impostor_draws", "support.clear()",
                         "input.resident_live_terrain = &support", "input.submitted_live_terrain = &support",
                         "fs::create_directories", 'partial << "LM"', "ctx.frozen_replay = replay"):
            self.assertIn(required, regression)

    def test_cold_far_supported_frame_prepares_frozen_mesh_fallback(self):
        source = read(RENDERING / "passes/GBufferPass.cpp")
        collect = body(source, "for (const std::size_t propIndex : candidates)")
        self.assertIn("if (impostor_candidate && !ctx.frozen_replay)", collect)
        warm = body(collect, "if (impostor_candidate && !ctx.frozen_replay)")
        self.assertIn("resolve_static_prop_lod(ctx, input, cp, 2)", warm)
        self.assertLess(collect.index("if (impostor_candidate && !ctx.frozen_replay)"),
                        collect.index("if (submission == StaticPropSubmission::Impostor)"))
        resolver = body(source, "GBufferPass::resolve_static_prop_lod(")
        self.assertIn("m_resolveMemo.find(rkey)", resolver)
        self.assertIn("m_resolveMemo", body(resolver, "if (rit == m_resolveMemo.end())"))
        self.assertIn("const auto rit = resolve_static_prop_lod(ctx, input, cp, lod);", collect)
        regression = body(read(ROOT / "test/rendering/render_smoke_test.cpp"),
                          "TEST(RenderSmokeTest, RefusedTreeImpostorsSubmitAvailableMeshLods)")
        cold = regression[regression.index("input.legacy_far = &far;"):
                          regression.index("input.legacy_far = nullptr;")]
        self.assertIn("ASSERT_TRUE(far.terrain_resident_at(root))", regression)
        self.assertEqual(cold.count("pass.execute(ctx, entities, input)"), 2)
        self.assertLess(cold.index("ctx.frozen_replay = false"), cold.index("pass.execute"))
        self.assertLess(cold.index("pass.execute"), cold.index("input.far_selection.submit = false"))
        self.assertLess(cold.index("input.far_selection.submit = false"), cold.index("ctx.frozen_replay = true"))
        for assertion in ("EXPECT_EQ(capture.impostor_draws, 1u)",
                          "EXPECT_EQ(capture.impostor_draws, 0u)",
                          "EXPECT_EQ(capture.mesh_draws, highest_lod >= 0 ? 3u : 0u)",
                          "EXPECT_EQ(capture.mesh_indices, 3 * (highest_lod + 1))"):
            self.assertIn(assertion, cold)

    def test_preview_support_and_draw_skip_camera_outer_clip_together(self):
        source = read(RENDERING / "FarLodSystem.cpp")
        query = compact(body(source, "bool FarLodSystem::terrain_resident_at("))
        self.assertIn("!m_preview_mode && (!std::isfinite(distance) || distance > m_outer_clip_m)", query)
        draw = body(source, "void FarLodSystem::draw_gbuffer(")
        preview = body(draw, "if (m_preview_mode)")
        for uniform in ("u_farClipFarRadius", "u_farClipNearRadius", "u_farClipInnerRadius"):
            self.assertIn('geometry_shader.setFloat("' + uniform + '", 0.0f)', preview)
        regression = body(read(ROOT / "test/rendering/far_lod_worker_test.cpp"),
                          "TEST(FarLodWorker, ElevatedCameraSeesTerrainInsideItsOwnRegion)")
        for assertion in ("ASSERT_GT(glm::length(preview_tree - camera), far.outer_clip_m())",
                          "EXPECT_FALSE(far.terrain_resident_at(preview_tree))",
                          "far.set_preview_anchor(preview_tree, 0.0f)",
                          "EXPECT_TRUE(far.terrain_resident_at(preview_tree))",
                          "far.set_preview_anchor(preview_tree, 1.0f)",
                          "far.set_enabled(false)", "far.clear_preview_anchor()"):
            self.assertIn(assertion, regression)

    def test_bulk_submitted_coverage_reuses_storage_and_binary_searches(self):
        source = read(RENDERING / "FarLodSystem.cpp")
        bulk = body(source, "void SubmittedLiveTerrain::assign_origins(")
        self.assertIn("m_submitted_chunks.reserve(origins.size())", bulk)
        self.assertIn("live_chunk_key(glm::vec3(origin), key)", bulk)
        self.assertIn("m_submitted_chunks.emplace_back(key, true)", bulk)
        self.assertIn("std::sort(m_submitted_chunks.begin(), m_submitted_chunks.end())", bulk)
        self.assertIn("std::unique(m_submitted_chunks.begin(), m_submitted_chunks.end())", bulk)
        self.assertNotIn("m_chunks.insert", bulk)
        clear = body(source, "void SubmittedLiveTerrain::clear(")
        self.assertIn("m_submitted_chunks.clear()", clear)
        for forbidden in ("shrink_to_fit", "swap", "reserve", "resize"):
            self.assertNotIn(forbidden, clear)
        query = body(source, "bool SubmittedLiveTerrain::covers(")
        self.assertIn("index < m_submitted_chunks.size() && m_submitted_chunks[index].second", compact(query))
        search = body(source, "std::size_t SubmittedLiveTerrain::submitted_index(")
        self.assertIn("std::lower_bound", search)
        self.assertIn("found->first == chunk_id", search)
        frame = body(self.pipeline, "void RenderPipeline::execute_stage_gbuffer(")
        self.assertNotIn("live.add_origin", frame)
        self.assertIn("live.assign_origins(std::span<const glm::vec4>(m_mdi_origin_scratch.data(), draws))", compact(frame))
        regression = body(read(ROOT / "test/rendering/render_smoke_test.cpp"),
                          "TEST(RenderSmokeTest, SubmittedLiveTerrainBulkFramesRetainCapacityAndReplaceCoverage)")
        for assertion in ("live.assign_origins({})", "std::reverse", "subspan(4)",
                          "EXPECT_EQ(live.submission_capacity(), capacity)",
                          "live.remove_chunk", "EXPECT_FALSE(live.covers({32, 1, 0}))"):
            self.assertIn(assertion, regression)

    def assert_live_submission_wiring(self):
        source = read(RENDERING / "FarLodSystem.cpp")
        self.assertTrue("bool SubmittedLiveTerrain::covers(" in source,
                        "live support must be derived from this frame's submitted chunks")
        query = body(source, "bool SubmittedLiveTerrain::covers(")
        self.assertIn("if (live_chunk_key(position, key) && contains(key))", query)
        self.assertIn("std::nextafter", query)
        self.assertIn("live_chunk_key(below, key) && contains(key)", query)
        self.assertNotIn("for (", query)
        self.assertNotIn("while (", query)
        key = body(source, "bool live_chunk_key(")
        for axis in ("x", "y", "z"):
            self.assertIn("std::isfinite(position." + axis + ")", key)
        self.assertIn("Chunk::calculate_id", key)
        for bound in ("kPackedMinXz", "kPackedMaxXz", "kPackedMinY", "kPackedMaxY"):
            self.assertIn(bound, key)
        add = body(source, "void SubmittedLiveTerrain::add_origin(")
        self.assertIn("if (live_chunk_key(origin, key))", add)
        self.assertIn("m_chunks.insert(key)", add)
        self.assertEqual(compact(body(source, "void SubmittedLiveTerrain::clear(")),
                         "m_chunks.clear(); m_submitted_chunks.clear();")
        submit = body(self.pipeline, "SubmitTerrainChunksFn RenderPipeline::make_terrain_submitter(")
        self.assertNotIn("m_farlod", submit)
        frame = body(self.pipeline, "void RenderPipeline::execute_stage_gbuffer(")
        for branch in (submit, frame):
            self.assertIn("m_hierarchicalCuller.CullHierarchical(", branch)
            self.assertIn("draw_chunks_mdi(visible_chunks, draws, indices)", branch)
            self.assertIn("m_terrain_coverage_visible.insert(chunk->id)", branch)
            self.assertIn("TerrainSubmitStats{visible_chunks.size(), draws, indices}", branch)
        self.assertLess(frame.index("draw_chunks_mdi(visible_chunks, draws, indices)"), frame.index("live.assign_origins("))
        self.assertIn("live.assign_origins(std::span<const glm::vec4>(m_mdi_origin_scratch.data(), draws))", compact(frame))
        self.assertLess(frame.index("live.clear();"), frame.index("live.assign_origins("))
        self.assertLess(frame.index("live.clear();"), frame.index("m_gbuffer_pass->execute("))
        self.assertIn("gbuffer_input.submitted_live_terrain = &live", frame)
        collect = body(read(RENDERING / "passes/GBufferPass.cpp"),
                       "for (const std::size_t propIndex : candidates)")
        self.assertIn("input.submitted_live_terrain && input.submitted_live_terrain->covers(cp.position)", compact(collect))
        self.assertNotIn("kFarClipInnerRadiusMeters", collect)
        return source

    def test_live_submitted_tree_at_200_metres_survives_cold_far_streaming(self):
        self.assert_live_submission_wiring()
        tests = read(ROOT / "test/rendering/render_smoke_test.cpp")
        self.assertIn("200.0f", body(tests, "TEST(RenderSmokeTest, StaticPropCollectionPreservesSubmittedLiveTerrainAtEveryLod)"))

    def test_tree_without_live_or_far_support_is_refused_even_inside_old_ring(self):
        source = self.assert_live_submission_wiring()
        policy = body(source, "StaticPropSubmission SelectStaticPropSubmission(")
        self.assertIn("!far_terrain_resident", policy)
        self.assertIn("return StaticPropSubmission::Refused;", policy)

    def test_far_only_support_survives_live_submission_loss(self):
        self.assert_live_submission_wiring()
        tests = read(ROOT / "test/rendering/render_smoke_test.cpp")
        transition = body(tests, "TEST(RenderSmokeTest, StaticPropCollectionKeepsFarSupportAfterLiveSubmissionDisappears)")
        for retained in ("live.clear()", "live.covers(position), true", "StaticPropSubmission::Impostor",
                         "live.covers(position), false", "StaticPropSubmission::Refused"):
            self.assertIn(retained, transition)

    def test_submitted_live_coverage_includes_192_384_640_lod_bands(self):
        self.assert_live_submission_wiring()
        tests = read(ROOT / "test/rendering/render_smoke_test.cpp")
        bands = body(tests, "TEST(RenderSmokeTest, StaticPropCollectionPreservesSubmittedLiveTerrainAtEveryLod)")
        for boundary in ("191.99f", "192.0f", "192.01f", "383.99f", "384.0f", "384.01f", "639.99f", "640.0f"):
            self.assertIn(boundary, bands)

    def assert_resident_live_support_wiring(self):
        frame = body(self.pipeline, "void RenderPipeline::execute_stage_gbuffer(")
        self.assertIn("gbuffer_input.resident_live_terrain = &m_farlod->resident_live_terrain();", frame)
        self.assertNotIn("resident_live_terrain().clear()", frame)
        self.assertNotIn("resident_live_terrain().add_origin(", frame)
        collect = body(read(RENDERING / "passes/GBufferPass.cpp"),
                       "for (const std::size_t propIndex : candidates)")
        ownership = compact(collect[collect.index("const bool live_terrain_owned"):
                                    collect.index("const bool far_terrain_resident")])
        self.assertIn("impostor_candidate &&", ownership)
        self.assertIn("|| (input.resident_live_terrain && input.resident_live_terrain->covers(cp.position))", ownership)
        for forbidden in ("selection", "distance", "dist <", "legacy_far", "for (", "while ("):
            self.assertNotIn(forbidden, ownership)
        inputs = read(RENDERING / "RenderInputs.h")
        self.assertIn("const SubmittedLiveTerrain* resident_live_terrain = nullptr;", inputs)

    def test_culled_ground_keeps_visible_canopy_support(self):
        self.assert_resident_live_support_wiring()
        tests = read(ROOT / "test/rendering/render_smoke_test.cpp")
        regression = body(tests, "TEST(RenderSmokeTest, VisibleCanopyKeepsResidentSupportWhenGroundIsFrustumCulled)")
        for required in ("TerrainCullingTestPeer::Cull({ground, canopy}, planes)",
                         "ASSERT_FALSE(visible.contains(Chunk::calculate_id(ground)))",
                         "ASSERT_TRUE(visible.contains(Chunk::calculate_id(canopy)))",
                         "submitted.clear()", "ASSERT_FALSE(submitted.covers(root))",
                         "ASSERT_TRUE(resident.covers(root))", "StaticPropSubmission::Impostor"):
            self.assertIn(required, regression)

    def test_authored_tree_band_keeps_resident_live_support_when_far_refuses(self):
        self.assert_resident_live_support_wiring()
        tests = read(ROOT / "test/rendering/render_smoke_test.cpp")
        regression = body(tests, "TEST(RenderSmokeTest, AuthoredTreeBandUsesResidentLiveTerrainWithoutFarSupport)")
        for required in ("65.0f", "100.0f", "175.99f", "176.0f", "640.0f",
                         "AuthoredTreeLodConfig(true)", "ASSERT_FALSE(far.terrain_resident_at(root))",
                         "ASSERT_TRUE(resident.covers(root))", "StaticPropSubmission::Impostor",
                         "StaticPropSubmission::Refused", "FarWorldRefusal::BackendUnavailable"):
            self.assertIn(required, regression)

    def test_live_residency_follows_successful_uploads_and_all_retirement_paths(self):
        upload = body(self.pipeline, "void RenderPipeline::upload_chunk_mesh(")
        publish = "m_farlod->resident_live_terrain().add_origin("
        self.assertEqual(upload.count(publish), 1)
        self.assertLess(upload.index("if (!payload.has_mesh())"), upload.index(publish))
        failure = body(upload, "if (new_handle == ChunkRenderData::kInvalidPoolHandle)")
        self.assertNotIn("resident_live_terrain", failure)
        self.assertIn("return;", failure)
        self.assertLess(upload.index("render_data.element_count = index_count;"), upload.index(publish))
        self.assertLess(upload.index("render_data.pool_handle = new_handle;"), upload.index(publish))
        self.assertNotIn("resident_live_terrain", body(self.pipeline, "void RenderPipeline::upload_water_mesh("))
        unload = body(self.pipeline, "void RenderPipeline::unload_chunk_resources(")
        self.assertIn("resident_live_terrain().remove_chunk(chunk_id)", unload)
        self.assertIn("submitted_live_terrain().remove_chunk(chunk_id)", unload)
        clear = body(self.pipeline, "void RenderPipeline::clear_all_chunk_data(")
        self.assertIn("resident_live_terrain().clear()", clear)
        self.assertIn("submitted_live_terrain().clear()", clear)
        far = read(RENDERING / "FarLodSystem.cpp")
        swap = body(far, "void FarLodSystem::prepare_world_swap(")
        self.assertNotIn("m_resident_live_terrain.clear()", swap)
        shutdown = body(far, "void FarLodSystem::shutdown(")
        self.assertIn("prepare_world_swap();", shutdown)
        self.assertIn("m_resident_live_terrain.clear()", shutdown)
        self.assertIn("m_farlod->shutdown();", body(self.pipeline, "void RenderPipeline::cleanup_gpu_resources("))
        removal = body(far, "void SubmittedLiveTerrain::remove_chunk(")
        self.assertIn("m_chunks.erase(chunk_id);", removal)
        self.assertIn("submitted_index(chunk_id)", removal)
        self.assertIn("m_submitted_chunks[index].second = false", removal)
        self.assertNotIn("m_submitted_chunks.erase", removal)

    def test_missing_live_and_far_terrain_still_refuses_tree(self):
        self.assert_resident_live_support_wiring()
        tests = read(ROOT / "test/rendering/render_smoke_test.cpp")
        regression = body(tests, "TEST(RenderSmokeTest, VisibleCanopyKeepsResidentSupportWhenGroundIsFrustumCulled)")
        self.assertLess(regression.index("resident.remove_chunk("),
                        regression.index("StaticPropSubmission::Refused"))
        self.assertIn("EXPECT_FALSE(resident.covers(root))", regression)
        policy = body(read(RENDERING / "FarLodSystem.cpp"), "StaticPropSubmission SelectStaticPropSubmission(")
        self.assertIn("if (!selection.submit || !far_terrain_resident)", policy)
        self.assertIn("return StaticPropSubmission::Refused;", policy)

    def test_ladder_impostors_refuse_missing_or_unknown_terrain_coverage(self):
        source = read(RENDERING / "FarLodSystem.cpp")
        self.assertIn("StaticPropSubmission SelectStaticPropSubmission(", source)
        policy = compact(body(source,
                              "StaticPropSubmission SelectStaticPropSubmission("))
        self.assertIn("if (!selection.submit || !far_terrain_resident)", policy)
        self.assertNotIn("selection.effective", policy)
        self.assertLess(policy.index("if (live_terrain_owned)"),
                        policy.index("if (!selection.submit || !far_terrain_resident)"))
        collect = body(read(RENDERING / "passes/GBufferPass.cpp"),
                       "for (const std::size_t propIndex : candidates)")
        far_support = compact(collect[collect.index("const bool far_terrain_resident"):
                                      collect.index("const auto submission")])
        self.assertIn("input.far_selection.effective == FarWorldMode::Legacy", far_support)
        self.assertIn("input.legacy_far &&", far_support)
        self.assertIn("input.legacy_far->terrain_resident_at(cp.position)", far_support)
        pipeline = read(RENDERING / "RenderPipeline.cpp")
        self.assertIn("gbuffer_input.legacy_far = legacy_far_compatibility();", pipeline)
        tests = read(ROOT / "test/rendering/render_smoke_test.cpp")
        regression = body(tests, "TEST(RenderSmokeTest, LadderImpostorsRequireConfirmedTerrainCoverage)")
        for required in ("ASSERT_EQ(ladder.effective, FarWorldMode::Ladder)",
                         "ladder, true, false, false", "ladder, true, false, true",
                         "far.resident_live_terrain().covers(root), false",
                         "StaticPropSubmission::Impostor", "StaticPropSubmission::Refused",
                         "resident_live_terrain().remove_chunk("):
            self.assertIn(required, regression)

    def test_near_terrain_eligibility_publishes_with_matching_mesh_version(self):
        world = read(ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        self.assertIn("CaptureNearTerrainMeshEvidence(", world)
        capture = body(world, "CaptureNearTerrainMeshEvidence(")
        self.assertIn("evidence->mesh_version = mesh_version;", capture)
        for axis in ("x", "y", "z"):
            self.assertIn(f"for (int {axis} = -1; {axis} <= 1; ++{axis}", capture)
        for refusal in ("!neighbour", "ChunkState::Loading", "ChunkState::Unloaded",
                        "ChunkState::Unloading", "kPackedMinXz", "kPackedMaxXz",
                        "kPackedMinY", "kPackedMaxY", "neighbour->has_terrain_authority()",
                        "neighbour->voxel_revision() != 0", "kFullSdfLattice", "kHeightmapCells"):
            self.assertIn(refusal, capture)
        boot = body(world, "bool SHIELD_WorldSystem::EnsureSurfaceReadyNear(")
        self.assertLess(boot.index("chunk->near_terrain_snapshot.store({}"),
                        boot.index("chunk->mesh_version++"))
        boot_publish = body(boot, "for (const auto& built : chunks_to_build) {\n        const auto& chunk")
        self.assertIn("chunk->mesh_version.load(std::memory_order_acquire)", boot_publish)
        self.assertIn("World::CompleteNearTerrainMeshSnapshot(*evidence, densities)", boot_publish)
        self.assertIn("auto& densities = built.step == 1 ? chunk->sdf_data : empty_densities;",
                      boot_publish)
        self.assertNotIn("densities = chunk->sdf_data", boot_publish)
        self.assertGreater(boot.index(boot_publish), boot.index("AddBoundaryTransitionSkirts("))
        dispatch = body(world, "SHIELD_WorldSystem::dispatch_meshing_jobs(")
        self.assertLess(dispatch.index("CaptureNearTerrainMeshEvidence("),
                        dispatch.index("lane_jobs.emplace_back("))
        worker = body(dispatch, "lane_jobs.emplace_back(")
        self.assertLess(worker.index("GenerateWaterMesh("),
                        worker.index("World::CompleteNearTerrainMeshSnapshot(*evidence, scratch.sdf_data)"))
        self.assertLess(worker.index("World::CompleteNearTerrainMeshSnapshot("),
                        worker.index("pending_mesh_ready.store(true"))
        publish = body(world, "bool SHIELD_WorldSystem::publish_front_meshing_batch(")
        terrain = body(publish, "if (job_chunk.terrain_mesh_required && terrain_current)")
        self.assertLess(terrain.index("chunk->mesh_version++"),
                        terrain.index("chunk->near_terrain_snapshot.store("))
        self.assertIn("job_chunk.scratch->near_terrain_snapshot.load(", terrain)
        snapshots = body(self.pipeline, "RenderPipeline::build_chunk_snapshots(")
        self.assertIn("chunk->near_terrain_snapshot.load(std::memory_order_acquire)", snapshots)
        self.assertIn("snapshot.near_terrain_snapshot.reset();",
                      body(snapshots, "if (version_after_metadata != snapshot.mesh_version)"))
        self.assertEqual(self.header.count("std::shared_ptr<const World::NearTerrainMeshSnapshot>"), 2)

    def test_near_terrain_frame_capture_defers_dependency_validation(self):
        snapshots = body(self.pipeline, "RenderPipeline::build_chunk_snapshots(")
        self.assertIn("chunk->near_terrain_snapshot.load(std::memory_order_acquire)", snapshots)
        for validation in ("CurrentNearTerrainMeshSnapshot(", "dependencies_current(",
                           "->matches(", "->eligible(", ".halo", "->halo"):
            self.assertNotIn(validation, snapshots)

    def test_near_terrain_edits_invalidate_self_and_every_halo_neighbour(self):
        chunk = read(ROOT / "src/luminumbra_common/world/Chunk.h")
        for marker in ("mark_voxel_data_dirty", "mark_sdf_loaded_or_edited",
                       "mark_sdf_generated_current_params", "normalize_for_park", "reset_pending_jobs"):
            self.assertIn("invalidate_near_terrain_snapshot();", body(chunk, f"void {marker}()"))
        invalidation = body(chunk, "void invalidate_near_terrain_snapshot()")
        self.assertIn("near_terrain_snapshot.store({},", invalidation)
        self.assertIn("near_terrain_epoch.compare_exchange_weak(", invalidation)
        self.assertIn("epoch != std::numeric_limits<u64>::max()", invalidation)
        world = read(ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        external = body(world, "void SHIELD_WorldSystem::notify_far_lod_authority_changed(")
        self.assertIn("chunk->invalidate_near_terrain_snapshot();", external)
        for setter in ("set_seed", "set_params", "clear_world"):
            self.assertIn("chunk->invalidate_near_terrain_snapshot();",
                          body(world, f"void SHIELD_WorldSystem::{setter}("))
        adoption = body(world, "bool SHIELD_WorldSystem::adopt_streamed_chunk(")
        self.assertIn("chunk->invalidate_near_terrain_snapshot();", body(adoption, "if (adopted)"))
        edit = body(world, "int SHIELD_WorldSystem::EditTerrainVoxel(")
        self.assertIn("chunk->mark_voxel_data_dirty();", body(edit, "if (changed)"))
        carve = read(ROOT / "src/luminumbra_client/core/scenarios/PersistenceRoundtrip.cpp")
        self.assertIn("chunk->mark_voxel_data_dirty();", carve)
        self.assertIn("world_system->notify_far_lod_authority_changed(chunk->get_coords());", carve)

    def test_near_terrain_water_promotion_invalidates_before_changing_provenance(self):
        chunk = read(ROOT / "src/luminumbra_common/world/Chunk.h")
        promotion = body(chunk, "void set_water_authority()")
        self.assertIn("sdf_provenance() != ChunkSdfProvenance::LoadedOrEdited", promotion)
        self.assertIn("voxel_revision() == 0", promotion)
        self.assertLess(promotion.index("invalidate_near_terrain_snapshot();"),
                        promotion.index("m_sdf_provenance.store("))
        self.assertIn("set_water_authority();", body(chunk, "void publish_water_state()"))

    def test_near_terrain_retirement_precedes_every_resident_removal(self):
        world = read(ROOT / "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp")
        activation = body(world, "bool SHIELD_WorldSystem::update_chunk_activation(")
        removals = activation.split("m_streaming_state.chunks.erase(it)")
        self.assertEqual(len(removals), 4)
        for prefix in removals[:-1]:
            self.assertTrue("invalidate_near_terrain_snapshot();" in prefix or
                            "normalize_for_park();" in prefix)
        restore = body(world, "bool SHIELD_WorldSystem::restore_authoritative_chunk(")
        discard = body(restore, "const auto discard_placeholder =")
        self.assertLess(discard.index("resident->second->invalidate_near_terrain_snapshot();"),
                        discard.index("m_streaming_state.chunks.erase(resident)"))
        for marker in ("void SHIELD_WorldSystem::clear_world(",
                       "SHIELD_WorldSystem::~SHIELD_WorldSystem()"):
            self.assertIn("chunk->invalidate_near_terrain_snapshot();", body(world, marker))
        streaming = read(ROOT / "src/luminumbra_common/world/WorldStreamingState.cpp")
        erase = body(streaming, "bool WorldStreamingState::erase_chunk(ChunkID id)")
        self.assertIn("it->second->invalidate_near_terrain_snapshot();", erase)
        self.assertLess(erase.index("it->second->invalidate_near_terrain_snapshot();"),
                        erase.index("m_chunks.erase(it)"))
        clear = body(streaming, "void WorldStreamingState::clear()")
        self.assertIn("chunk->invalidate_near_terrain_snapshot();", clear)
        self.assertLess(clear.index("chunk->invalidate_near_terrain_snapshot();"),
                        clear.index("m_chunks.clear()"))
        self.assertNotIn("WorldStreamingState::~WorldStreamingState()", streaming)

    def test_near_terrain_lattice_retention_requires_explicit_refinement_request(self):
        contract = read(ROOT / "src/luminumbra_common/world/NearTerrainMeshSnapshot.h")
        self.assertIn("bool retain_densities = false", contract)
        complete = body(contract, "CompleteNearTerrainMeshSnapshot(")
        retention = body(complete, "if (retain_densities &&")
        self.assertIn("snapshot->densities = std::move(densities);", retention)
        self.assertEqual(complete.count("snapshot->densities ="), 1)
        self.assertNotIn("densities.size()", body(contract, "bool\n    eligible("))

    def test_near_terrain_stale_records_are_rejected_through_upload_queue(self):
        contract_path = ROOT / "src/luminumbra_common/world/NearTerrainMeshSnapshot.h"
        try:
            contract = read(contract_path)
        except FileNotFoundError:
            self.fail("near-terrain snapshot contract is missing")
        dependencies = body(contract, "bool dependencies_current() const")
        self.assertIn("for (const auto& dependency : halo)", dependencies)
        self.assertIn("!chunk", dependencies)
        self.assertIn("chunk->near_terrain_epoch.load(std::memory_order_acquire) != dependency.epoch",
                      compact(dependencies))
        matching = body(contract, "bool matches(")
        for guard in ("version != 0", "mesh_version == version", "dependencies_current()",
                      "chunk.near_terrain_snapshot.load(std::memory_order_acquire).get() == this",
                      "chunk.mesh_version.load(std::memory_order_acquire) == version"):
            self.assertIn(guard, matching)
        payload = body(self.pipeline, "bool RenderPipeline::copy_terrain_mesh_payload(")
        self.assertIn("payload.near_terrain_snapshot = chunk.near_terrain_snapshot;", payload)
        self.assertIn("!payload.near_terrain_snapshot->matches(*chunk.source_chunk, version_before)",
                      payload)
        self.assertEqual(payload.count("payload.near_terrain_snapshot.reset();"), 2)
        for upload in ("upload_chunk_mesh", "upload_water_mesh"):
            self.assertNotIn("RefineNearTerrain(",
                             body(self.pipeline, f"void RenderPipeline::{upload}("))
        native = read(ROOT / "test/rendering/NearTerrainRefinement_test.cpp")
        for regression in ("PublishesMatchingMeshVersionAndOwnsItsLattice",
                           "EveryEditInvalidatesSelfAndAllHaloNeighbours",
                           "WaterPromotionInvalidatesSourceAndEveryHaloNeighbour",
                           "ResidentRemovalRefusesRetainedOwnersAfterReplacement",
                           "LiveWorldEvictionRefusesRetainedOwnersAfterReplacement",
                           "EvidenceWithoutRefinementRetainsNoLattice",
                           "TemporaryViewsPreserveResidentEvidence",
                           "RejectsStaleQueuedRecordsAndRetiredOwners",
                           "RefusesMissingPartialEditedCoarseAndCaveEvidence"):
            self.assertIn("TEST(NearTerrainEligibility, " + regression + ")", native)
        self.assertIn("rendering/NearTerrainRefinement_test.cpp", read(ROOT / "test/CMakeLists.txt"))

    def test_peak_station_records_rendered_centre_and_target_mesh_diagnostics(self):
        capture = read(ROOT / "src/luminumbra_client/core/ScenarioRunnerCapture.cpp")
        player = body(capture, "void ScenarioRunnerImpl::capturePlayerView(")
        self.assertIn('if (capture.station.name == "peak")', player)
        peak = compact(body(player, 'if (capture.station.name == "peak")'))
        self.assertEqual(peak, "capture.terrain_peak = "
                         "renderPipeline.terrain_peak_diagnostics(capture.station.target);")
        self.assertLess(player.index("terrain_peak_diagnostics("),
                        player.index("writeScenarioPixels("))
        writer = read(ROOT / "src/luminumbra_client/core/scenarios/PlayerViewSmoke.cpp")
        self.assertIn('{"terrain_peak", capture.terrain_peak}', writer)
        source = read(RENDERING / "TerrainCoverageCapture.cpp")
        diagnostic = body(source, "nlohmann::json RenderPipeline::terrain_peak_diagnostics(")
        self.assertIn('out["target"] = describe({target.x, target.y, target.z});', diagnostic)
        self.assertIn('out["centre"] = describe({position.x, position.y, position.z});', diagnostic)
        for key in ("lod", "sample_step_m", "mesh_path", "terrain_owner",
                    "mesh_metadata_status", "far_mode", "far_draws"):
            self.assertIn('"' + key + '"', diagnostic)
        self.assertIn("const ReadbackState restore(gbuffer().fbo_id);", diagnostic)
        self.assertNotIn("glGetError(", diagnostic)
        self.assertIn("static_cast<double>(pixel_x) + 0.5", diagnostic)
        self.assertIn("projection[2][0] += m_taau_jitter_ndc.x;", diagnostic)
        self.assertIn("projection[2][1] += m_taau_jitter_ndc.y;", diagnostic)
        self.assertNotIn("get_renderable_chunks(", diagnostic)
        self.assertIn("std::min(snapshots.size(), kFarWorldLiveCellLimit)", diagnostic)
        native = read(ROOT / "test/rendering/FarWorldWaterGl_test.cpp")
        for regression in ("PeakDiagnosticsObserveCentreDepthAndStableMeshInputs",
                           "PeakDiagnosticsDoNotGuessPendingUnknownOrInvalidMeshPaths"):
            self.assertIn("TEST_F(FarWorldWaterGl, " + regression + ")", native)

    def test_peak_ownership_preserves_legacy_overlap_and_incomplete_evidence(self):
        source = read(RENDERING / "TerrainCoverageCapture.cpp")
        self.assertIn("nlohmann::json RenderPipeline::terrain_peak_diagnostics(", source)
        diagnostic = compact(body(source, "nlohmann::json RenderPipeline::terrain_peak_diagnostics("))
        live = re.search(r"const bool exclusive_live = (.*?);", diagnostic).group(1)
        far = re.search(r'else if \((!presented.*?)\) \{ '
                        r'point\["terrain_owner"\] = "far";', diagnostic).group(1)

        def expression(text):
            for before, after in (
                ("m_frame_far_selection.effective == FarWorldMode::Ladder", "ladder"),
                ("m_frame_far_selection.effective != FarWorldMode::Ladder", "(not ladder)"),
                ("m_last_render_pass_stats.far_region_draws", "draws"),
                ("snapshots.size() == count", "complete_search"),
                ("ownership.complete()", "complete_mask"),
                ("&&", " and "), ("||", " or "),
            ):
                text = text.replace(before, after)
            return re.sub(r"!(?!=)", " not ", text).strip()

        live, far = expression(live), expression(far)
        # All 128 boolean combinations, including absent/stale uploads,
        # incomplete masks/searches and Legacy overlap, use the shipping predicates.
        for bits in range(128):
            presented, inner, ladder, ladder_owned, complete_mask, complete_search, drawn = (
                bool(bits & (1 << bit)) for bit in range(7))
            state = dict(presented=presented, inner=inner, ladder=ladder,
                         ladder_owned=ladder_owned, complete_mask=complete_mask,
                         complete_search=complete_search, draws=int(drawn))
            actual = "unresolved"
            if presented and eval(live, {"__builtins__": {}}, state):
                actual = "live"
            elif eval(far, {"__builtins__": {}}, state):
                actual = "far"
            expected = "unresolved"
            if presented and (inner or not drawn or (ladder and ladder_owned)):
                expected = "live"
            elif (not presented and complete_search and not inner and drawn and
                  (not ladder or (complete_mask and not ladder_owned))):
                expected = "far"
            self.assertEqual(actual, expected, state)

    def test_wiring_regression_runs_in_ctest_and_python_ci(self):
        registration = read(ROOT / "test/CMakeLists.txt")
        self.assertEqual(registration.count("add_test(NAME UploadBudgetWiringPython "), 1)
        self.assertEqual(registration.count("add_executable(upload_budget_test "), 1)
        targets = registration.split("set(LUMINUMBRA_GTEST_TARGETS", 1)[1].split(")", 1)[0]
        self.assertEqual(targets.split().count("upload_budget_test"), 1)
        ci_paths = read(ROOT / "tools/ci/ci_paths.py")
        python_tests = ci_paths.split("PYTHON_TESTS = (", 1)[1].split(")", 1)[0]
        self.assertIn('"test/rendering/test_upload_budget_wiring.py"', python_tests)

    def test_save_fence_preserves_current_world_until_quit_save_succeeds(self):
        quit_action = body(self.main, '} else if (act == "quit")')
        save = quit_action.index("if (!gameSession->SaveWorld())")
        fence = "renderPipeline.wait_for_accepted_far_tile_writes();"
        self.assertIn(fence, quit_action[:save])
        self.assertIn("return;", body(quit_action, "if (!gameSession->SaveWorld())"))
        failure = body(quit_action, "if (!gameSession->SaveWorld())")
        self.assertNotIn("prepare_world_entry()", failure)
        cleanup = quit_action.index("prepare_world_entry();")
        self.assertGreater(cleanup, save + len("if (!gameSession->SaveWorld())") + len(failure))
        self.assertEqual(quit_action.count("prepare_world_entry();"), 1)
        for destructive in ("prepare_world_entry()", "prepare_world_swap()",
                            "DrainWorldDressing(", "DrainBackgroundWorldScan("):
            self.assertNotIn(destructive, quit_action[:save])
        benchmark = body(self.main, "[&](bool main_menu)")
        self.assertIn(fence, benchmark[:benchmark.index("gameSession->SaveWorld()")])
        cleanup_scope = body(benchmark, "if (saved && main_menu)")
        self.assertIn("prepare_world_entry();", cleanup_scope)
        self.assertEqual(benchmark.count("prepare_world_entry();"), 1)
        forward = compact(body(self.pipeline, "void RenderPipeline::wait_for_accepted_far_tile_writes()"))
        self.assertEqual(forward, "m_far_world->WaitForAcceptedCacheWrites(); "
                         "if (m_farlod) m_farlod->wait_for_accepted_cache_writes();")
        legacy = read(RENDERING / "FarLodSystem.cpp")
        self.assertEqual(compact(body(legacy, "void FarLodSystem::wait_for_accepted_cache_writes()")),
                         "m_tile_saves.flush();")
        for backend, check in (("FarWorldRenderer", "CheckOwner();"),
                               ("FarWorldHandoffRenderer", "Guard const guard(*this);")):
            source = read(RENDERING / (backend + ".cpp"))
            wait = body(source, "void " + backend + "::WaitForAcceptedCacheWrites()")
            self.assertEqual(compact(re.sub(r"//[^\n]*", "", wait)), check)

    def test_fallible_world_open_waits_without_discarding_until_revision_changes(self):
        wait = compact(body(self.main, "auto wait_for_world_entry_readers ="))
        self.assertEqual(wait, "renderPipeline.wait_for_world_readers(); "
                         "WaitForBackgroundWorldScan(jobSystem); jobSystem.wait(s_worldDressingHandle);")
        cleanup = body(self.main, "auto finish_world_replacement =")
        guard = "if (gameSession->WorldRevision() != entry_world_revision)"
        self.assertIn(guard, cleanup)
        self.assertEqual(compact(cleanup.split(guard, 1)[1]),
                         "{ release_player_controller(); prepare_world_entry(); }")
        # Both outcomes use the same revision predicate: refusal before adoption
        # preserves state; success and a failure after adoption both retire it.
        expression = re.search(r"if \((.*)\)", cleanup).group(1)
        expression = expression.replace("gameSession->WorldRevision()", "current")
        for previous, current, expected in ((7, 7, False), (7, 8, True)):
            self.assertEqual(eval(expression, {"__builtins__": {}},
                                  dict(current=current, entry_world_revision=previous)), expected)
        for signature, call, outcome in (
                ("auto start_world_creation =", "gameSession->CreateWorld(", "if (!created)"),
                ("auto start_world_load =", "gameSession->LoadWorldFrom(", "if (!loaded)")):
            opening = body(self.main, signature)
            self.assertNotIn("prepare_world_entry();", opening)
            self.assertLess(opening.index("wait_for_world_entry_readers();"), opening.index(call))
            self.assertLess(opening.index(call), opening.index("finish_world_replacement();"))
            self.assertLess(opening.index("finish_world_replacement();"), opening.index(outcome))
        stages = body(self.main, "auto run_entry_stage =")
        readers = stages.split("case WorldEntryStage::DrainReaders:", 1)[1].split(
            "case WorldEntryStage::OpenWorld:", 1)[0]
        self.assertIn("wait_for_world_entry_readers();", readers)
        self.assertNotIn("prepare_world_entry();", readers)
        opening = stages.split("case WorldEntryStage::OpenWorld:", 1)[1].split(
            "case WorldEntryStage::RestoreState:", 1)[0]
        self.assertLess(opening.index("gameSession->AdoptPreparedWorld("),
                        opening.index("finish_world_replacement();"))
        self.assertLess(opening.index("finish_world_replacement();"), opening.index("if (!opened)"))
        backdrop = body(self.main, "if (want_backdrop)")
        self.assertLess(backdrop.index("wait_for_world_entry_readers();"),
                        backdrop.index("gameSession->CreateTransientWorld("))
        self.assertLess(backdrop.index("finish_world_replacement();"), backdrop.index("if (created"))
        pipeline_wait = body(self.pipeline, "void RenderPipeline::wait_for_world_readers()")
        self.assertIn("m_farlod->wait_for_world_readers();", pipeline_wait)
        self.assertIn("m_far_world->WaitForAcceptedCacheWrites();", pipeline_wait)
        legacy = read(RENDERING / "FarLodSystem.cpp")
        legacy_wait = body(legacy, "void FarLodSystem::wait_for_world_readers()")
        self.assertIn("m_job_system->wait(handle);", legacy_wait)
        self.assertIn("wait_for_accepted_cache_writes();", legacy_wait)
        for destructive in ("m_world =", "m_epoch", "m_pending.clear()", "m_ready_builds.clear()",
                            "m_shared->completed.clear()"):
            self.assertNotIn(destructive, legacy_wait)
        scan = read(ROOT / "src/luminumbra_client/app/CaveFlourishes.cpp")
        scan_wait = body(scan, "void WaitForBackgroundWorldScan(")
        self.assertIn("jobs.wait(state.handle);", scan_wait)
        self.assertNotIn("state.scan.reset()", scan_wait)

    def test_waterfall_replacement_clears_sites_and_spray_before_empty_site_return(self):
        source = read(RENDERING / "passes/WaterfallPass.cpp")
        destroy = compact(body(source, "void WaterfallPass::destroy_geometry()"))
        self.assertIn("m_sites.clear();", destroy)
        self.assertEqual(brace_depth(destroy, destroy.index("m_sites.clear();")), 0)
        build = compact(body(source, "WaterfallPass::build_geometry("))
        self.assertLess(build.index("m_sites.clear();"), build.index("waterfall_sites(world)"))
        begin = compact(body(source, "void WaterfallPass::begin_adoption("))
        preserve = begin.index("auto prepared_sites = std::move(m_sites);")
        destroy_prepared = begin.index("destroy_geometry();")
        restore = begin.index("m_sites = std::move(prepared_sites);")
        self.assertLess(preserve, destroy_prepared)
        self.assertLess(destroy_prepared, restore)
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

    def test_native_save_fences_release_owners_before_driver_destruction(self):
        native = read(ROOT / "test/rendering/FarWorldRenderer_test.cpp")
        handoff = body(native, "TEST(FarWorldHandoffRenderer, SaveFencePreservesBindingOwnershipAndDraws)")
        calls = ("renderer.PrepareWorldSwap();", "EXPECT_EQ(state.live, 0u);",
                 "EXPECT_TRUE(state.released_with_context);", "state.fixture_released = true;",
                 "renderer.Shutdown();", "jobs.shutdown();")
        positions = [handoff.index(call) for call in calls]
        self.assertEqual(positions, sorted(positions))
        fixture = body(native, "TEST(FarWorldRenderer, SaveFencePreservesBindingOwnershipAndDraws)")
        self.assertLess(fixture.index("renderer.PrepareWorldSwap();"),
                        fixture.index("EXPECT_TRUE(owner.State().fixture_released);"))
        driver = body(native, "~Driver() override")
        self.assertIn("EXPECT_TRUE(state_.fixture_released);", driver)
        self.assertIn("EXPECT_EQ(state_.live, 0u);", driver)

    def test_shutdown_joins_entry_worker_before_first_destructive_drain(self):
        shutdown = self.main.split('trace_shutdown("query_cleanup_started");', 1)[1]
        cancel = shutdown.index("world_entry.cancel();")
        advance = shutdown.index("world_entry.advance(run_entry_stage, drain_entry);")
        first_swap = shutdown.index("renderPipeline.prepare_world_swap();")
        self.assertLess(cancel, advance)
        self.assertLess(advance, first_swap)
        self.assertLess(advance, shutdown.index("DrainBackgroundWorldScan(jobSystem);"))
        self.assertLess(advance, shutdown.index("DrainWorldDressing("))
        drain = body(self.main, "auto drain_entry =")
        self.assertLess(drain.index("jobSystem.wait(entry_cpu_job);"),
                        drain.index("prepare_world_entry();"))
        self.assertLess(drain.index("jobSystem.wait(entry_cpu_job);"),
                        drain.index("renderPipeline.cancel_waterfall_adoption();"))

    def test_live_regeneration_retires_far_results_and_rebuilds_waterfalls(self):
        viewer = read(ROOT / "src/luminumbra_client/debug/WorldGenViewer.cpp")
        apply = body(viewer, 'if (ImGui::Button("Apply & Regenerate"))')
        calls = ("pipeline.prepare_world_swap();", "main_world_system->set_params(m_params);",
                 "main_world_system->set_seed(m_seed);",
                 "main_world_system->regenerate_all_chunks(nullptr);",
                 "pipeline.prepare_waterfalls(*main_world_system);")
        positions = [apply.index(call) for call in calls]
        self.assertEqual(positions, sorted(positions))
        swap = body(self.pipeline, "void RenderPipeline::prepare_world_swap()")
        self.assertIn("m_far_world->PrepareWorldSwap();", swap)
        self.assertIn("m_farlod->prepare_world_swap();", swap)
        legacy = body(read(RENDERING / "FarLodSystem.cpp"),
                      "void FarLodSystem::prepare_world_swap()")
        self.assertIn("wait_for_builds();", legacy)
        self.assertIn("++m_epoch;", legacy)

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

    def test_preview_full_reset_invalidates_pass_owned_ambient_cache(self):
        source = read(RENDERING / "passes/ParticlePass.cpp")
        clear = body(source, "void ParticlePass::clear_emitters()")
        self.assertIn("m_active_emitters.clear();", clear)
        self.assertIn("m_ambient_emitter_index = kMaxEmitters;", clear)
        update = body(source, "void ParticlePass::update_ambient_motes(")
        create = body(update, "if (m_ambient_emitter_index == kMaxEmitters)")
        self.assertIn('add_emitter(root_path / "data/common/particles/ambient_motes.json",', create)
        self.assertIn("kInvalidEmitter)", create)
        self.assertIn("m_ambient_emitter_index = m_active_emitters.size() - 1;", create)
        self.assertIn("m_active_emitters[m_ambient_emitter_index]", update)
        self.assertIn("emitter.world_origin = camera_position + emitter.data.origin;", update)
        self.assertIn("particles->update_ambient_motes(root_dir, g_camera->Position);", self.main)
        self.assertNotIn("ambientMotes", self.main)
        preview = read(ROOT / "src/luminumbra_client/world/WorldgenPreview.cpp")
        for signature in ("void WorldgenPreview::apply_precipitation(",
                          "void WorldgenPreview::clear_precipitation("):
            self.assertIn("particles->clear_emitters();", body(preview, signature))
        cancel = body(self.pipeline, "void RenderPipeline::cancel_waterfall_adoption()")
        self.assertIn("m_particle_pass->clear_emitters();", cancel)
        native = read(ROOT / "test/rendering/particle_determinism_test.cpp")
        self.assertIn("TEST_F(ParticleLiveTest, AmbientMotesTickAfterPreviewFullReset)", native)
        self.assertIn("rendering/particle_determinism_test.cpp", read(ROOT / "test/CMakeLists.txt"))

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
        self.assertIn("manage_chunk_gpu_resources(renderable_chunk_snapshots, camera, snapshot_ms, presentation)",
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
                        render.index("dispatch_stages(camera, deferred_only_capture)"))
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
        draw = body(self.pipeline, "void RenderPipeline::execute_stage_opaque_snapshot(")
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
        self.assertIn("(!Water && !retire_refined) || found == records.end() ||", compact(integrate))
        self.assertIn("found->second.element_count == 0", integrate)
        self.assertIn("bool retire_refined = false", integrate)
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

    def test_terrain_diagnostic_refusal_clears_before_batched_fallback(self):
        attributed = body(self.pipeline, "void SubmitDiagnosticTerrainBucket(")
        refusal = compact(body(attributed, "if (resident.lod < 0 || resident.lod > 255)"))
        self.assertLess(refusal.index('unsupported("terrain_resident_lod_unavailable")'),
                        refusal.index("capture->clear_current_identity();"))
        self.assertLess(refusal.index("capture->clear_current_identity();"),
                        refusal.index("capture = nullptr;"))
        # A table-full refusal clears, drops the capture and leaves the loop the same way.
        table_full = compact(body(attributed, "if (capture->refused())"))
        self.assertTrue(table_full.endswith(
            "capture->clear_current_identity(); capture = nullptr; break;"), table_full)
        self.assertEqual(attributed.count("break;"), 2)
        draw = body(self.pipeline, "void RenderPipeline::draw_chunks_mdi(")
        excess = compact(body(draw, "if (diagnostic_capture && running > 65536)"))
        self.assertLess(excess.index("clear_current_identity();"),
                        excess.index("diagnostic_capture = nullptr;"))
        staged = body(draw, "if (diagnostic_chunks)")
        self.assertIn("SubmitDiagnosticTerrainBucket(", staged)
        self.assertIn("continue;", staged)

    def test_terrain_diagnostic_identity_precedes_each_indirect_command(self):
        attributed = body(self.pipeline, "void SubmitDiagnosticTerrainBucket(")
        loop = compact(body(attributed, "for (std::size_t draw = first;"))
        self.assertLess(loop.index("capture->write_current("),
                        loop.index("AttributedGl::Submit( glDrawElementsIndirect,"))
        self.assertIn(".lod = static_cast<std::uint32_t>(resident.lod)", loop)
        self.assertIn("const auto& resident = draws[draw - first]", loop)

    def test_terrain_generated_mesh_key_includes_resident_geometry_generation(self):
        draw = body(self.pipeline, "void RenderPipeline::draw_chunks_mdi(")
        self.assertIn(".geometry_generation = a.geometry_generation", draw)
        attributed = body(self.pipeline, "void SubmitDiagnosticTerrainBucket(")
        key = compact(attributed[attributed.index("std::string mesh_key;"):
                                 attributed.index("capture->write_current(")])
        self.assertEqual(key,
                         'std::string mesh_key; mesh_key.reserve(94); mesh_key += "terrain/"; '
                         "mesh_key += std::to_string(resident.chunk[0]); mesh_key += '/'; "
                         "mesh_key += std::to_string(resident.chunk[1]); mesh_key += '/'; "
                         'mesh_key += std::to_string(resident.chunk[2]); mesh_key += "/version/"; '
                         'mesh_key += std::to_string(resident.mesh_version); '
                         'mesh_key += "/generation/"; '
                         'std::array<char, 20> generation{}; // uint64 has at most 20 decimal digits. '
                         'const auto* generation_end = std::to_chars(generation.data(), '
                         'generation.data() + generation.size(), resident.geometry_generation) .ptr; '
                         'mesh_key.append(generation.data(), '
                         'static_cast<std::size_t>(generation_end - generation.data()));')
        regression = read(ROOT / "test/rendering/diagnostic_attribution_test.cpp")
        self.assertIn("TerrainGeometryIdentityTracksUploadsAcrossEvictionAndRedraw", regression)

    def test_terrain_geometry_generation_covers_both_upload_paths_and_never_resets(self):
        pool = read(RENDERING / "passes/ChunkGeometryPool.cpp")
        self.assertIn("u64 next_geometry_generation()", pool)
        sequence = compact(body(pool, "u64 next_geometry_generation()"))
        self.assertIn("static u64 generation = 0;", sequence)
        self.assertIn("if (generation == std::numeric_limits<u64>::max()) { return 0; }",
                      sequence)
        self.assertTrue(sequence.endswith("return ++generation;"), sequence)
        self.assertEqual(pool.count("next_geometry_generation();"), 2)
        for signature in ("u32 ChunkGeometryPool::allocate_with_reclaim(",
                          "u32 ChunkGeometryPool::reuse_completed("):
            upload = compact(body(pool, signature))
            self.assertIn("const u64 geometry_generation = next_geometry_generation();", upload)
            self.assertIn("if (geometry_generation == 0) { return kInvalid; }", upload)
            self.assertLess(upload.index("if (geometry_generation == 0)"),
                            upload.index("std::memcpy("))
            self.assertIn("alloc.geometry_generation = geometry_generation;", upload)
            self.assertLess(upload.rindex("std::memcpy("),
                            upload.index("alloc.geometry_generation = geometry_generation;"))
        for signature in ("void ChunkGeometryPool::free(", "void ChunkGeometryPool::clear()",
                          "void ChunkGeometryPool::destroy()"):
            self.assertNotIn("generation", body(pool, signature))
        header = read(RENDERING / "passes/ChunkGeometryPool.h")
        self.assertIn("u64 geometry_generation = 0;", header)

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
        attributed = body(self.pipeline, "void SubmitDiagnosticTerrainBucket(")
        diagnostic = compact(body(attributed, "for (std::size_t draw = first;"))
        self.assertLess(diagnostic.index("AttributedGl::Submit( glDrawElementsIndirect,"),
                        diagnostic.index("retirement.submitted = true;"))
        # A table-full refusal leaves the loop before its command is drawn individually.
        self.assertLess(diagnostic.index("capture->refused()"),
                        diagnostic.index("AttributedGl::Submit( glDrawElementsIndirect,"))
        # Undiagnosed draws, and the rest of a bucket after a diagnostic refusal, share one multi-draw.
        batched = compact(body(attributed, "if (mdi_first < first + count)"))
        self.assertLess(batched.index("AttributedGl::Submit( glMultiDrawElementsIndirect,"),
                        batched.index("retirement.submitted = true;"))
        self.assertNotIn("pool.mark_submitted(", diagnostic)
        self.assertNotIn("pool.mark_submitted(", batched)
        # One fence-tracked retirement per bucket, also on an exception after a draw.
        retire = compact(body(attributed, "~BucketRetirement()"))
        self.assertIn("if (submitted) { pool.mark_submitted(bucket); }", retire)
        self.assertEqual(compact(attributed).count("pool.mark_submitted("), 1)
        self.assertEqual(compact(self.pipeline).count("AttributedGl::Submit( glMultiDrawElementsIndirect,"), 2)
        shipping = compact(body(draw, "if (mdi_first < first + count)"))
        self.assertLess(shipping.index("glMultiDrawElementsIndirect,"),
                        shipping.index("m_chunk_geometry_pool.mark_submitted("))
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
        self.assertIn("ResizeSimulationGrid(*cp, target_level); continue;", migration)
        resize = compact(body(source, "bool WaterSystem::ResizeSimulationGrid("))
        self.assertIn("new_resolution == current_resolution && "
                      "!(chunk.water_bed_mm.empty() && chunk.water_depth_mm.empty())", resize)
        update = compact(body(source, "void WaterSystem::update("))
        self.assertIn("chunk_ptr->water_bed_mm.empty() && chunk_ptr->water_depth_mm.empty()",
                      update)
        native = read(ROOT / "test/common/WaterSeedInputs_test.cpp")
        for regression in ("SameResolutionLegacyFloatsMigrateAtLoadAndDirectResize",
                           "SameResolutionLegacyFloatsMigrateDuringLiveUpdate",
                           "SameResolutionIntegerAdoptionRebuildsMirrorsForQueriesAndSprings",
                           "IntegerMirrorRepairPreservesSizedMirrorsAndWellFormedSavedBytes",
                           "IntegerMirrorRepairRefusesPartialAuthorityAndChunksWithoutSimulation"):
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

    def test_parity_captures_refuse_frame_errors_before_allocations_can_consume_them(self):
        for name in ("frame", "upscale_seam", "ssao"):
            capture = body(self.pipeline, "bool RenderPipeline::capture_" + name + "_parity(")
            check = capture.index("if (glGetError() != GL_NO_ERROR)")
            refusal = body(capture, "if (glGetError() != GL_NO_ERROR)")
            self.assertEqual(compact(refusal), "return false;")
            self.assertLess(capture.index("CapturePixelBytes("), check)
            first_work = "RenderContext ctx" if name == "ssao" else "auto make_target"
            self.assertLess(check, capture.index(first_work))
            self.assertLess(check, capture.index("std::filesystem::create_directories("))
        resize = body(self.pipeline, "bool RenderPipeline::resize_graphics_targets(")
        self.assertIn("m_render_registry.replace_targets(", resize)
        registry = read(RENDERING / "RenderResourceRegistry.cpp")
        allocation = body(registry, "bool allocate_texture_storage(")
        self.assertIn("while (glGetError() != GL_NO_ERROR)", allocation)

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
        self.assertIn('if (!capture_finished) capture_failure(g_app.capture.frame_scan_path, "frame_capture_incomplete")', scan)
        self.assertLess(scan.index("capture_ok = image_written"), scan.index("WriteFrameScanReport("))

    def test_early_close_and_sibling_images_cannot_return_partial_capture_success(self):
        main = compact(self.main)
        self.assertIn("g_app.capture.frame_scan_active && !frame_scan_complete && exit_code == 0", main)
        self.assertIn("g_app.capture.survey_active && !survey_complete && exit_code == 0", main)
        self.assertIn("!scene_capture_complete && exit_code == 0", main)
        self.assertIn("frame_scan_complete = capture_finished;", main)
        self.assertIn("survey_complete = true;", main)
        self.assertIn("if (!scene_capture_complete) capture_failure(", main)
        self.assertIn("if (!screenshot_complete) capture_failure(", main)
        timelapse = compact(body(self.main, "static bool CaptureTimelapseFrame("))
        self.assertIn("CapturePixelBytes(vw, vh, 3, pixel_bytes)", timelapse)
        self.assertIn("!WritePixelBufferPpm(g_app.capture.timelapse_dir / nm, vw, vh, px)) return false;", timelapse)
        self.assertIn("if (!CaptureTimelapseFrame(", main)
        self.assertIn("g_app.capture.timelapse_captured < g_app.capture.timelapse_frames && exit_code == 0", main)

    def test_capture_manifests_wrap_every_requested_capture_before_readback(self):
        survey = body(read(RENDERING / "SceneSurvey.cpp"), "bool RunSceneSurvey(")
        timelapse = body(self.main, "static bool CaptureTimelapseFrame(")
        scene = body(self.main, "if (g_app.capture.scene_active &&")
        scan = body(self.main, "if (g_app.capture.frame_scan_active && currentState == GameState::IN_GAME &&")
        for source in (survey, timelapse, scene, scan):
            self.assertEqual(source.count("WithCaptureManifest("), 1)
            self.assertEqual(source.count("ObserveCaptureManifest("), 1)
            self.assertLess(source.index("WithCaptureManifest("), source.index("glReadPixels("))
        self.assertLess(survey.index("WithCaptureManifest("), survey.index("ScanFrame("))
        self.assertLess(scan.index("WithCaptureManifest("), scan.index("ScanFrame("))
        self.assertNotIn("set_time_of_day(", scan)
        self.assertNotIn("g_camera->Position =", scan)
        self.assertIn("frame_scan_manifest,", scan)
        self.assertIn("if (!captured) return false;", compact(survey))
        self.assertIn("if (!captured) return false;", compact(timelapse))
        wrapper = compact(body(read(RENDERING / "CaptureManifest.h"), "bool WithCaptureManifest("))
        self.assertIn("if (!manifest.Begin()) return false;", wrapper)
        self.assertLess(wrapper.index("manifest.Begin()"), wrapper.index("write_capture()"))
        self.assertLess(wrapper.index("write_capture()"), wrapper.index("manifest.Finish(captured)"))
        self.assertIn("return captured && finished;", wrapper)
        live = read(RENDERING / "CaptureManifestLive.cpp")
        for forbidden in ("TickSimulation(", "wait_for_streaming_jobs(", "snapshot_streamed_chunks(",
                          "glRead", "std::ifstream", "std::filesystem::exists"):
            self.assertNotIn(forbidden, live)
        for query in ("GetEngineVersionString()", "PresetContentHash()", "GetSimulationTickCount()",
                      "GetMetadata().seed", "pipeline.capture_camera()", "SetCaptureManifestCamera(input, *camera)",
                      "graphics_runtime_config()", "taau_jitter_ndc()"):
            self.assertIn(query, live)
        diagnostic = compact(body(self.header, "const FarReplayCamera* capture_camera() const"))
        self.assertEqual(diagnostic,
                         "return m_frame_prepared.valid ? &m_frame_prepared.replay_camera : nullptr;")
        self.assertIn("if (!camera || !SetCaptureManifestCamera(input, *camera))", live)

    def test_capture_manifest_builder_and_lifecycle_regressions_are_registered_without_gl(self):
        cmake = read(ROOT / "test/CMakeLists.txt")
        target = cmake.split("add_executable(capture_manifest_test", 1)[1].split(")", 1)[0]
        self.assertIn("rendering/capture_manifest_test.cpp", target)
        self.assertIn("rendering/CaptureManifest.cpp", target)
        targets = cmake.split("set(LUMINUMBRA_GTEST_TARGETS", 1)[1].split(")", 1)[0]
        self.assertEqual(targets.split().count("capture_manifest_test"), 1)
        native = read(ROOT / "test/rendering/capture_manifest_test.cpp")
        for regression in ("FixedClockIsByteIdenticalAndKeysAreLexicographic",
                           "MatricesRoundTripFloat32AndStateIsUnchanged",
                           "PreparedCameraSurvivesLivePoseChangesAndRefusesInvalidSnapshots",
                           "UnavailableIdentitiesRequireReasonsAndNeverInventHashes",
                           "IncompletePrecedesPayloadAndFinalVerdictFollowsIt",
                           "PayloadFailureAndAbandonmentCannotPublishComplete",
                           "StaleTemporaryAndInvalidParentRefuseWithoutCallingPayload",
                           "FinalPublicationFailureRetainsIncompleteEvidence",
                           "RenameFailureNeverDeletesTheDestinationOrTemporaryEvidence",
                           "InvalidNumbersExtentsAndMetadataAreRefusedBeforeWriting"):
            self.assertIn("TEST_F(CaptureManifestTest, " + regression + ")", native)
        for source in ("CaptureManifest.cpp", "CaptureManifestLive.cpp"):
            self.assertIn("/rendering/" + source, read(ROOT / "src/luminumbra_client/sources.cmake"))
        builder = read(RENDERING / "CaptureManifest.cpp")
        self.assertIn("MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)", builder)
        self.assertIn("std::filesystem::rename(temporary, path, error)", builder)
        self.assertIn('std::fopen(temporary.c_str(), "wbx")', builder)
        self.assertIn('_wfopen_s(&output, temporary.c_str(), L"wbx")', builder)


class SkyLutContinuity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.source = read(RENDERING / "SkyAtmosphereLut.ipp")

    def test_cpu_fallback_repairs_empty_partial_and_oversized_base_luts_before_sampling(self):
        sky = body(self.source, "void SkyAtmosphereLut::build_sky_view_cpu(")
        prefix = sky.split("const glm::vec3 beta_r =", 1)[0]
        header = read(RENDERING / "SkyAtmosphereLut.h")
        constants = {name: int(value) for name, value in re.findall(
            r"static constexpr int (k\w+) = (\d+);", header)}
        sizes = {"m_transmittance_cpu": 256 * 64, "m_multiscatter_cpu": 32 * 32}
        guards = re.findall(r"if\s*\((.*?)\)\s*\{\s*(build_\w+_cpu)\(\);\s*\}",
                            prefix, re.S)
        self.assertEqual([builder for _, builder in guards],
                         ["build_transmittance_cpu", "build_multiscatter_cpu"])
        # Execute the production size predicates for all 25 shape pairs. Empty
        # covers GPU-only startup; short/long covers incomplete or stale buffers;
        # exact sizes must reuse the existing CPU output without another build.
        for trans in (0, 1, 256 * 64 - 1, 256 * 64, 256 * 64 + 1):
            for multi in (0, 1, 32 * 32 - 1, 32 * 32, 32 * 32 + 1):
                state = dict(m_transmittance_cpu=trans, m_multiscatter_cpu=multi)
                rebuilt = []
                for predicate, builder in guards:
                    expression = compact(predicate).replace("static_cast<std::size_t>", "int")
                    expression = expression.replace(".size()", "")
                    if eval(expression, {"__builtins__": {}}, dict(constants, int=int, **state)):
                        if builder == "build_multiscatter_cpu":
                            self.assertEqual(state["m_transmittance_cpu"], sizes["m_transmittance_cpu"])
                        buffer = "m_" + builder.removeprefix("build_")
                        state[buffer] = sizes[buffer]
                        rebuilt.append(buffer)
                with self.subTest(trans=trans, multi=multi):
                    self.assertEqual(state, sizes)
                    self.assertEqual(rebuilt, [name for name, count in
                                              (("m_transmittance_cpu", trans),
                                               ("m_multiscatter_cpu", multi))
                                              if count != sizes[name]])

    def test_every_cpu_entry_and_gpu_failure_converges_on_guarded_builder(self):
        # There are only two CPU entry sites, shared by all platforms/backends.
        self.assertEqual(self.source.count("build_sky_view_cpu("), 3)
        for method, branch in (("initialize", "if (!gpu_init_done)"),
                               ("refresh_sky_view", "if (!gpu_done)")):
            entry = body(self.source, "bool SkyAtmosphereLut::" + method + "(")
            self.assertIn("build_sky_view_cpu(", body(entry, branch))
        gpu = body(self.source, "bool SkyAtmosphereLut::build_sky_view_gpu(")
        failure = body(gpu, "if (!readback_ok)")
        self.assertIn("return false;", failure)
        self.assertIn("mapped_bytes >= skyview_bytes", gpu)
        self.assertIn("std::chrono::steady_clock::now() >= readback_deadline", gpu)
        # Resize/quality never changes the fixed LUT geometry or clears its CPU
        # storage; a subsequent refresh still enters the same guarded builder.
        pipeline = read(RENDERING / "RenderPipeline.cpp")
        for method in ("void RenderPipeline::on_resize(",
                       "bool RenderPipeline::resize_graphics_targets(",
                       "bool RenderPipeline::apply_graphics(",
                       "void RenderPipeline::set_cloud_quality("):
            self.assertNotIn("m_sky_lut", body(pipeline, method))

    def test_gpu_success_then_fallback_regressions_are_registered_without_skips(self):
        registration = read(ROOT / "test/CMakeLists.txt")
        target = registration.split("add_executable(render_smoke_test", 1)[1].split(")", 1)[0]
        self.assertIn("rendering/SkyAtmosphereLutGl_test.cpp", target)
        native = read(ROOT / "test/rendering/SkyAtmosphereLutGl_test.cpp")
        for name in ("GpuInitializationThenReadbackDeadlineMatchesCpu",
                     "GpuInitializationThenCpuModeMatchesCpu",
                     "InitializationShaderFailureMatchesCpu",
                     "InitializationReadbackDeadlineMatchesCpu"):
            self.assertIn("TEST_F(SkyAtmosphereLutGl, " + name + ")", native)
        self.assertNotIn("GTEST_SKIP", native)
        self.assertIn("EXPECT_EQ(Pixels(actual), Pixels(reference));", native)
        self.assertIn("EXPECT_EQ(lut.sun_transmittance(0.0f), glm::vec3(1.0f));", native)

    def cpu_sampler(self, width, height, values):
        if "sample_lut_linear(" not in self.source:
            # Model the old production lookup too, so the numeric regressions
            # fail on its radiance step rather than on a missing helper name.
            trans = compact(body(self.source, "glm::vec3 SkyAtmosphereLut::sample_transmittance("))
            self.assertIn("static_cast<int>(u * kTransmittanceWidth)", trans)
            self.assertIn("static_cast<int>(v * kTransmittanceHeight)", trans)

            def nearest(u, v):
                x = min(width - 1, int(min(1.0, max(0.0, u)) * width))
                y = min(height - 1, int(min(1.0, max(0.0, v)) * height))
                return values[y * width + x]

            return nearest
        helper = compact(body(self.source, "sample_lut_linear("))
        self.assertIn("const int x0 = static_cast<int>(x);", helper)
        self.assertIn("const int y0 = static_cast<int>(y);", helper)
        self.assertIn("const int x1 = std::min(x0 + 1, width - 1);", helper)
        self.assertIn("const int y1 = std::min(y0 + 1, height - 1);", helper)
        self.assertIn("lut[static_cast<std::size_t>(row) * static_cast<std::size_t>(width) + "
                      "static_cast<std::size_t>(column)]", helper)

        def expression(text):
            text = text.replace("glm::clamp", "clamp").replace("glm::mix", "mix")
            text = text.replace("static_cast<float>", "float")
            return re.sub(r"(?<=\d)f\b", "", text)

        axes = [expression(re.search(r"const float " + axis + r" = (.*?);", helper).group(1))
                for axis in ("x", "y")]
        blend = expression(re.search(r"return (glm::mix\(.*);$", helper).group(1))
        functions = {"float": float, "clamp": lambda x, lo, hi: min(hi, max(lo, x)),
                     "mix": lambda a, b, t: a * (1.0 - t) + b * t}

        def sample(u, v):
            scope = dict(functions, width=width, height=height, u=u, v=v)
            x, y = [eval(axis, {"__builtins__": {}}, scope) for axis in axes]
            x0, y0 = int(x), int(y)
            scope.update(x=x, y=y, x0=x0, y0=y0,
                         x1=min(x0 + 1, width - 1), y1=min(y0 + 1, height - 1),
                         at=lambda column, row: values[row * width + column])
            return eval(blend, {"__builtins__": {}}, scope)

        return sample

    def test_cpu_and_both_compute_builders_use_continuous_nonperiodic_sampling(self):
        trans = compact(body(self.source, "glm::vec3 SkyAtmosphereLut::sample_transmittance("))
        self.assertIn("return sample_lut_linear(m_transmittance_cpu, kTransmittanceWidth, "
                      "kTransmittanceHeight, u, v);", trans)
        sky = compact(body(self.source, "void SkyAtmosphereLut::build_sky_view_cpu("))
        self.assertIn("sample_lut_linear(m_multiscatter_cpu, kMultiScatterWidth, "
                      "kMultiScatterHeight, ms_u, ms_v)", sky)
        self.assertNotIn("static_cast<int>", sky)
        for builder in ("build_multiscatter_cpu", "build_sky_view_cpu"):
            self.assertIn("sample_transmittance(", body(self.source, "void SkyAtmosphereLut::" + builder))
        for name in ("kSkyViewComputeSrc", "kMultiscatterComputeSrc"):
            shader = self.source.split('const char* ' + name + ' = R"GLSL(', 1)[1].split(')GLSL"', 1)[0]
            with self.subTest(builder=name):
                helper = compact(body(shader, "vec3 sampleLutLinear("))
                self.assertEqual(helper, "vec2 halfTexel = 0.5 / vec2(textureSize(lut, 0)); "
                                 "return textureLod(lut, clamp(uv, halfTexel, "
                                 "vec2(1.0) - halfTexel), 0.0).rgb;")
                self.assertIn("return sampleLutLinear(u_transmittance, vec2(u, v));",
                              body(shader, "vec3 sampleTransmittance("))
                self.assertNotIn("texelFetch(", shader)
                if name == "kSkyViewComputeSrc":
                    self.assertIn("sampleLutLinear(u_multiscatter, vec2(msU, msV))", shader)
        # Linear filtering is required in both CPU-upload and GPU-allocation paths.
        for signature in ("void SkyAtmosphereLut::upload_texture(", "void allocate_empty_lut_texture("):
            self.assertIn("GL_TEXTURE_MIN_FILTER, GL_LINEAR", body(self.source, signature))
            self.assertIn("GL_TEXTURE_MAG_FILTER, GL_LINEAR", body(self.source, signature))

    def test_production_bilinear_mapping_centres_edges_and_every_old_cell_boundary(self):
        # These are the two bounded LUT dimensions, not a per-frame allocation.
        for width, height in ((256, 64), (32, 32)):
            values = [float((x * 17 + y * 7) % 31) for y in range(height) for x in range(width)]
            sample = self.cpu_sampler(width, height, values)
            for x, y in ((0, 0), (width // 2, height // 2), (width - 1, height - 1)):
                self.assertEqual(sample((x + 0.5) / width, (y + 0.5) / height), values[y * width + x])
            for u, v, index in ((-1.0, -1.0, 0), (0.0, 0.0, 0),
                                (1.0, 1.0, -1), (2.0, 2.0, -1)):
                self.assertEqual(sample(u, v), values[index])
            # GPU normalized coordinates clamp before filtering, while the CPU
            # clamps in texel space. Check agreement when just one axis clamps.
            for u in (-1.0, 0.0, 0.5 / width, 0.37, 1.0 - 0.5 / width, 1.0, 2.0):
                for v in (-1.0, 0.0, 0.5 / height, 0.63, 1.0 - 0.5 / height, 1.0, 2.0):
                    gu = min(1.0 - 0.5 / width, max(0.5 / width, u)) * width - 0.5
                    gv = min(1.0 - 0.5 / height, max(0.5 / height, v)) * height - 0.5
                    expected = sum(values[y * width + x] * wx * wy
                                   for x, wx in ((int(gu), 1.0 - gu % 1.0),
                                                 (min(int(gu) + 1, width - 1), gu % 1.0))
                                   for y, wy in ((int(gv), 1.0 - gv % 1.0),
                                                 (min(int(gv) + 1, height - 1), gv % 1.0)))
                    self.assertAlmostEqual(sample(u, v), expected)
            u, v = (3.5 + 0.25) / width, (4.5 + 0.75) / height
            expected = sum(values[y * width + x] * wx * wy
                           for x, wx in ((3, 0.75), (4, 0.25))
                           for y, wy in ((4, 0.25), (5, 0.75)))
            self.assertAlmostEqual(sample(u, v), expected)
            epsilon = 1e-10
            for axis, count in ((0, width), (1, height)):
                for cell in range(1, count):
                    before, after = [0.5, 0.5], [0.5, 0.5]
                    before[axis], after[axis] = cell / count - epsilon, cell / count + epsilon
                    self.assertLess(abs(sample(*before) - sample(*after)), 1e-5)

    def test_dusk_zero_cosine_meridian_is_continuous_and_matches_sweep_seam(self):
        # Summer, dusk and camera values are pinned to the shipping sweep inputs.
        sweep = read(ROOT / "src/luminumbra_client/core/scenarios/WorldVisualSweep.cpp")
        self.assertIn('{"dusk", 0.25f}', sweep)
        self.assertIn('{"yaw120", 120.0f, 0.0f, false', sweep)
        self.assertIn('camera->Zoom = 75.0f;', sweep)
        tilt = float(re.search(r"kSeasonalTiltAmplitude = ([0-9.]+)f",
                              read(RENDERING / "TimeOfDayModel.h")).group(1))
        sun_z = 0.2 - math.sin(tilt)
        bearing = math.degrees(math.atan2(sun_z, -1.0)) % 360.0
        meridian = bearing - 90.0
        focal = 800.0 / math.tan(math.radians(75.0 / 2.0))
        x = 1920.0 + focal * math.tan(math.radians(meridian - 120.0))
        self.assertAlmostEqual(x, 1566.0, delta=2.0)
        for width, height in ((256, 64), (32, 32)):
            # A nonconstant fixture isolates the bin-crossing discontinuity from
            # the integration. All march samples cross this boundary together.
            values = [float(x >= width // 2) for _ in range(height) for x in range(width)]
            sample = self.cpu_sampler(width, height, values)
            for zenith in (0.1, 0.5, 1.0, math.radians(89.0)):
                def mu(azimuth):
                    return 0.01 * math.sin(zenith) * math.cos(math.radians(azimuth - bearing))

                left = (mu(meridian - 1e-5) + 1.0) * 0.5
                right = (mu(meridian + 1e-5) + 1.0) * 0.5
                self.assertLess(abs(sample(left, 0.2) - sample(right, 0.2)), 1e-6)
                # Negative control: devel's nearest lookup has a full step here.
                old_left = values[int(left * width)]
                old_right = values[int(right * width)]
                self.assertEqual(abs(old_left - old_right), 1.0)


class DuskAttributionWiring(unittest.TestCase):
    def test_audit_pose_and_only_last_settled_frame_request_attribution(self):
        sweep = read(ROOT / "src/luminumbra_client/core/scenarios/WorldVisualSweep.cpp")
        self.assertIn("anchor(168.0f, 10.0505952835083f, -40.0f)", sweep)
        for token in ('{"dusk", 0.25f}', '{"yaw000", 0.0f, 0.0f, false',
                      '{"yaw240", 240.0f, 0.0f, false', 'camera->Zoom = 75.0f;',
                      'const int settle = WorldSweepStillSettleFrames(storm);', 'apply_weather(storm, phase_clock,'):
            self.assertIn(token, sweep)
        settled = compact(body(sweep, "const auto render_settled ="))
        self.assertIn("i + 1 == settle_frames ? attribution_dir : std::filesystem::path{}", settled)
        self.assertIn("if (!ok)", settled)
        selection = re.search(r"const bool dusk_attribution = (.*?);", compact(sweep)).group(1)
        self.assertEqual(selection, "tod.time_of_day == 0.25f && "
                         "(a.yaw_degrees == 0.0f || a.yaw_degrees == 240.0f) && !a.aim_at_water")
        self.assertIn('failures.push_back("missing_attribution:" + c.file)', sweep)
        self.assertIn('cell.attribution_complete = dusk_attribution && ok;', sweep)
        self.assertIn('pipeline->set_terrain_coverage_diagnostics_enabled(enabled, bypass)', sweep)
        self.assertIn('pipeline->set_terrain_coverage_diagnostics_enabled(true)', sweep)
        for key in ("attribution_directory", "attribution_complete", "cloud_phase",
                    "horizon_fov_degrees", "horizon_pitch_degrees"):
            self.assertIn('"' + key + '"', sweep)

    def test_matching_capture_precedes_present_and_propagates_failure(self):
        source = read(ROOT / "src/luminumbra_client/core/ScenarioRunnerCapture.cpp")
        hook = body(source, "void ScenarioRunnerImpl::onPreRenderWorldSweep()")
        readback = hook.index("glReadPixels(")
        self.assertIn("!renderPipeline.capture_terrain_coverage(attribution_dir)", hook)
        attribution = hook.index("!renderPipeline.capture_terrain_coverage(attribution_dir)")
        present = hook.index("glfwSwapBuffers(window)")
        self.assertLess(readback, attribution)
        self.assertLess(attribution, present)
        refusal = body(hook, "if (!attribution_dir.empty() &&")
        self.assertIn("return false;", refusal)
        self.assertIn("settle_uploads(", hook[:readback])

    def test_sources_are_bounded_and_completion_follows_every_readback(self):
        source = read(RENDERING / "TerrainCoverageCapture.cpp")
        capture = body(source, "bool RenderPipeline::capture_terrain_coverage(")
        for name in ("lit-scene", "opaque-scene", "sky-view", "transmittance", "ambient-ssao",
                     "shadow-depth", "shadow-tint", "fog-inscatter", "fog-transmittance"):
            self.assertIn('"' + name + '"', capture)
        for key in ("lighting_uniforms", "fog_uniforms", "ambient_color", "clouds", "weather",
                    "volumetric_quality", "unavailable_terms", "lighting_source_scope"):
            self.assertIn('"' + key + '"', capture)
        self.assertLess(capture.index("CaptureFog("), capture.index('receipt["complete"] = true'))
        self.assertLess(capture.index('receipt["complete"] = true'), capture.index("receipt.dump(2)"))
        self.assertLess(capture.index("max_pixels / m_internal_height"), capture.index("create_directory("))
        texture = compact(body(source, "bool CaptureTexture("))
        for guard in ("texture == 0", "width <= 0", "height <= 0", "layers < 1", "layers > 64",
                      "max_pixels / static_cast<std::size_t>(height)", "channels != 1",
                      "glGetError() != GL_NO_ERROR", "!WriteFloatPlane("):
            self.assertIn(guard, texture)
        self.assertLess(texture.index("max_pixels /"), texture.index("std::vector<float> values(count)"))
        for field in ("GL_PACK_IMAGE_HEIGHT", "GL_PACK_SKIP_IMAGES", "GL_PIXEL_PACK_BUFFER_BINDING",
                      "GL_PACK_SWAP_BYTES"):
            self.assertIn(field, source)
        replay = body(source, "bool CaptureFog(")
        self.assertIn('"aerial-inscatter"', replay)
        self.assertIn('"aerial-opacity"', replay)
        self.assertIn("glCheckNamedFramebufferStatus(", replay)
        self.assertNotIn("setFloat(", replay)
        self.assertNotIn("render_frame(", replay)
        for state in ("GL_DRAW_FRAMEBUFFER_BINDING", "GL_CURRENT_PROGRAM", "GL_VERTEX_ARRAY_BINDING",
                      "GL_VIEWPORT", "GL_COLOR_WRITEMASK", "GL_POLYGON_MODE", "GL_ACTIVE_TEXTURE"):
            self.assertIn(state, body(source, "struct FogReadback"))
        native = read(ROOT / "test/rendering/FarWorldFogGl_test.cpp")
        fixture = body(native, "TEST_F(FarWorldFogGl, DuskAuditFogIsContinuousAcrossDistanceAndHeightBoundaries)")
        for pin in ("ComputeSunGeometry(.25f, kSeasonalTiltAmplitude)", "{0.0f, 240.0f}",
                    "{false, true}", "Near(samples[0], samples[1], .002f)",
                    "EXPECT_GT(samples[0].a, .01f)"):
            self.assertIn(pin, fixture)
        self.assertIn("rendering/FarWorldFogGl_test.cpp", read(ROOT / "test/CMakeLists.txt"))


class SkyAppearanceWiring(unittest.TestCase):
    def test_ambient_tag_is_confined_to_render_instances(self):
        particles = read(RENDERING / "passes/ParticlePass.cpp")
        update = compact(body(particles, "void ParticlePass::update("))
        self.assertIn("rec.atlas_layer = emitter.data.atlas_layer;", update)
        self.assertIn("rec.rotation = p.emitter_index == m_ambient_emitter_index ? "
                      "std::numeric_limits<int8_t>::min() : "
                      "static_cast<int8_t>(std::lround(angle_norm * 127.0f));", update)
        setup = compact(body(particles, "void ParticlePass::init_buffers("))
        self.assertIn("kMaxInstances * sizeof(InstanceRecord)", setup)
        self.assertIn("glVertexAttribIFormat(6, 1, GL_BYTE, "
                      "offsetof(InstanceRecord, rotation))", setup)
        self.assertIn("glVertexAttribBinding(6, 0)", setup)
        # The magical fixture and the authored ambient descriptor keep layer 0.
        for filename in ("ambient_motes.json", "fixture_sparkle.json"):
            self.assertRegex(read(ROOT / "data/common/particles" / filename),
                             r'"atlas_layer"\s*:\s*0\b')
        for stage in ("vert", "geom", "frag"):
            source = read(ROOT / ("res/shaders/magical_particles." + stage))
            self.assertIn("flat float atlasLayer;", source)
            self.assertIn("flat uint ambientMote;", source)
        vertex = read(ROOT / "res/shaders/magical_particles.vert")
        self.assertIn("layout (location = 6) in int aAmbientMote;", vertex)
        self.assertIn("vs_out.ambientMote = aAmbientMote == -128 ? 1u : 0u;", vertex)
        geometry = read(ROOT / "res/shaders/magical_particles.geom")
        self.assertIn("gs_out.ambientMote = gs_in[0].ambientMote;", geometry)
        native = read(ROOT / "test/rendering/particle_determinism_test.cpp")
        self.assertIn("TEST_F(ParticleLiveTest, AmbientFlagPreservesEveryAuthoredLayer)", native)
        self.assertIn("layers{0, 1, 2, 3, 4, 65535}", native)

    def test_ambient_dust_scattering_rejects_sky_and_fades_with_distance(self):
        shader = read(ROOT / "res/shaders/magical_particles.frag")
        self.assertIn("bool isAmbientMote = fs_in.ambientMote != 0u;", shader)
        dust = compact(body(shader, "if (isAmbientMote)"))
        self.assertIn("sceneDepthRaw > 0.0 ? 1.0 : 0.0", dust)
        self.assertIn("1.0 - smoothstep(4.0, 24.0, fs_in.distanceToCamera)", dust)
        self.assertIn("finalColor.rgb *= clamp(lit, vec3(0.0), vec3(1.0));", dust)
        self.assertIn("finalColor.a *= shape * softFade * skyFade * distanceFade;", dust)
        self.assertNotIn("+=", dust)
        self.assertIn("else if (isRain || isSnow)", shader)
        native = body(read(ROOT / "test/rendering/render_smoke_test.cpp"),
                      "TEST(RenderSmokeTest, ReversedDepthSoftParticlesFadeAgainstGeometry)")
        for assertion in ("EXPECT_FLOAT_EQ(alpha(0), 0.0f)",
                          "EXPECT_FLOAT_EQ(alpha(32), 0.0f)",
                          "EXPECT_NEAR(alpha(3000), 1.0f", "EXPECT_NEAR(alpha(32), 0.5f",
                          "EXPECT_NEAR(probe.draw()[16].r, 0.25f",
                          "for (GLuint layer : {0u, 1u, 2u, 3u, 4u, 65535u})"):
            self.assertIn(assertion, native)

    def test_star_cell_hash_mixes_all_coordinates_without_lattice_repetition(self):
        shader = read(ROOT / "res/shaders/enhanced_skybox.frag")
        # Pin the complete integer algorithm so this mirror cannot silently drift.
        self.assertEqual(compact(body(shader, "uint starCellSeed(")), compact("""
            uvec3 p = uvec3(cell);
            uint seed = (p.x * 0x8da6b343u) ^ (p.y * 0xd8163841u) ^ (p.z * 0xcb1ab31fu);
            seed ^= seed >> 16u;
            seed *= 0x7feb352du;
            seed ^= seed >> 15u;
            seed *= 0x846ca68bu;
            return seed ^ (seed >> 16u);
        """))
        self.assertEqual(compact(body(shader, "float starRandom(")), compact("""
            seed ^= seed >> 16u;
            seed *= 0x7feb352du;
            seed ^= seed >> 15u;
            seed *= 0x846ca68bu;
            seed ^= seed >> 16u;
            return float(seed >> 8u) * (1.0 / 16777216.0);
        """))
        stars = compact(body(shader, "vec3 renderStars("))
        self.assertIn("vec3 p = viewDir * 320.0;", stars)
        self.assertIn("uint seed = starCellSeed(ivec3(floor(p)));", stars)
        self.assertIn("float brightness = starRandom(seed);", stars)
        for salt in ("0x9e3779b9u", "0x85ebca6bu", "0xc2b2ae35u"):
            self.assertIn("starRandom(seed ^ " + salt + ")", stars)

        def mix(seed):
            seed ^= seed >> 16
            seed = (seed * 0x7feb352d) & 0xffffffff
            seed ^= seed >> 15
            seed = (seed * 0x846ca68b) & 0xffffffff
            return seed ^ (seed >> 16)

        def cell_seed(cell):
            seed = 0
            for coordinate, multiplier in zip(cell, (0x8da6b343, 0xd8163841, 0xcb1ab31f)):
                seed ^= (coordinate * multiplier) & 0xffffffff
            return mix(seed)

        # Negative coordinates, zero and extrema around the old lattice
        # collision. All grid samples and their partners fit the live range.
        for x in range(-283, 321, 19):
            for y in range(-249, 321, 19):
                for z in range(-320, 320, 19):
                    first = cell_seed((x, y, z))
                    second = cell_seed((x - 37, y - 71, z + 1))
                    self.assertNotEqual(first, second, (x, y, z))
                    outputs = lambda seed: tuple(mix(seed ^ salt) >> 8 for salt in
                                                 (0, 0x9e3779b9, 0x85ebca6b, 0xc2b2ae35))
                    self.assertNotEqual(outputs(first), outputs(second), (x, y, z))
        for offset in ((-37, -71, 1), (37, 71, 1)):
            self.assertNotEqual(cell_seed((0, 0, 0)), cell_seed(offset), offset)
        for axis in range(3):
            for coordinate in range(-320, 321):
                cell = [0, 0, 0]
                cell[axis] = coordinate
                self.assertNotEqual(cell_seed(cell), cell_seed(
                    tuple(value - delta for value, delta in zip(cell, (37, 71, -1)))))

    def test_stars_have_a_horizon_cutoff_in_both_sky_palette_paths(self):
        shader = read(ROOT / "res/shaders/enhanced_skybox.frag")
        stars = compact(body(shader, "vec3 renderStars("))
        self.assertIn("nightIntensity < 0.1 || viewDir.y <= 0.0", stars)
        self.assertIn("float horizonFade = smoothstep(0.0, 0.1, viewDir.y);", stars)
        self.assertIn("* nightIntensity * horizonFade;", stars)
        # Stars use one path after LUT/fallback selection, rather than a capture override.
        main = body(shader, "void main()")
        self.assertGreater(main.index("skyColor += renderStars("),
                           main.index("skyColor = legacyGradient("))

    def test_stars_use_compact_radial_profiles_without_grid_noise_thresholds(self):
        shader = read(ROOT / "res/shaders/enhanced_skybox.frag")
        stars = compact(body(shader, "vec3 renderStars("))
        self.assertIn("starProfile(length(fract(p) - centre))", stars)
        self.assertIn("* 0.5 + 0.25;", stars)
        self.assertNotIn("noise(", stars)
        profile = compact(body(shader, "float starProfile("))
        # Evaluate the production scalar profile, including its support boundary.
        expression = re.fullmatch(r"return (.*);", profile).group(1)

        def smoothstep(low, high, value):
            t = min(1.0, max(0.0, (value - low) / (high - low)))
            return t * t * (3.0 - 2.0 * t)

        def sample(radius):
            return eval(expression, {"__builtins__": {}},
                        {"radius": radius, "smoothstep": smoothstep})

        self.assertEqual(sample(0.0), 1.0)
        previous = 1.0
        for index in range(1, 101):
            value = sample(index * 0.003)
            self.assertGreaterEqual(value, 0.0)
            self.assertLessEqual(value, previous)
            previous = value
        for radius in (0.18, 0.25, 0.5, 1.0):
            self.assertEqual(sample(radius), 0.0)
        self.assertGreater(sample(0.09), 0.0)

    def test_aurora_and_stars_share_cloud_transmittance(self):
        shader = read(ROOT / "res/shaders/enhanced_skybox.frag")
        main = body(shader, "void main()")
        cloud = main.index("skyColor = renderClouds(viewDir, skyColor, dayFactor);")
        for layer in ("renderAurora(viewDir, dayFactor)", "renderStars(viewDir, nightFactor)"):
            self.assertEqual(main.count(layer), 1)
            self.assertLess(main.index("skyColor += " + layer), cloud)
        clouds = body(shader, "vec3 renderClouds(")
        self.assertIn("return mix(baseColor, cloudColor, alpha);", clouds)
        self.assertIn("return baseColor;", clouds)

class NightLightingSourceTests(unittest.TestCase):
    def test_frame_scan_pins_override_live_clock_before_the_rendered_capture(self):
        source = read(ROOT / 'src/luminumbra_client/main_client.cpp')
        signature = 'if (g_app.capture.frame_scan_active && g_camera)'
        pin = body(source, signature)
        self.assertIn('renderPipeline.set_time_of_day(0.04f);', pin)
        self.assertIn('g_camera->Position = glm::vec3(8.0f, 24.0f, 8.0f);', pin)
        self.assertIn('g_camera->updateCameraVectors();', pin)
        self.assertNotIn('frame_scan_settle', pin)
        self.assertNotIn('render_uploads_settled', pin)
        position = source.index(signature)
        rendered = source.index('renderPipeline.render_frame(', position)
        captured = source.index('frame_scan_manifest =', rendered)
        self.assertLess(source.index('renderPipeline.set_world_clock(gameSession->GetWorldClock())'), position)
        self.assertLess(source.index('renderPipeline.set_time_of_day_tick(sim_tick)'), position)
        pin_end = source.index('{', position) + len(pin) + 2
        self.assertNotIn('set_time_of_day(', source[pin_end:rendered])
        self.assertLess(rendered, captured)

    def test_shipping_terrain_and_foliage_share_lunar_inputs_and_exposure_cap(self):
        source = (ROOT / 'src/luminumbra_client/rendering/RenderPipeline.cpp').read_text(encoding='utf-8')
        for name, following in (('make_foliage_context', 'make_lighting_context'),
                                ('make_lighting_context', 'make_skybox_context')):
            body = source.split('RenderContext RenderPipeline::' + name + '(', 1)[1].split(
                'RenderContext RenderPipeline::' + following + '(', 1)[0]
            self.assertIn('ctx.moon_radiance = m_moonRadiance * MoonLightFactor(1.0f, m_moonUpFactor)', body)
            self.assertIn('ctx.moon_illumination = m_moonIllumination', body)
            self.assertIn('m_auto_exposure_metered && m_metered_valid', body)
            self.assertIn('ctx.exposure = SelectRenderExposure(', body)
            self.assertIn('MoonScaledAutoExposure(', body)
            self.assertIn('-m_sun.direction.y, m_moonIllumination, m_moonUpFactor)', body)
        self.assertIn('NightAmbientColor(m_moonIllumination, m_moonUpFactor)', source)
        self.assertNotIn('kStarlightFrac', source)

    def test_forward_and_deferred_keys_use_the_scaled_radiance(self):
        for filename in ('lighting_pass.frag', 'foliage.frag'):
            source = (ROOT / 'res/shaders' / filename).read_text(encoding='utf-8')
            self.assertIn('const float kMoonKeyScale = 0.12;', source)
            self.assertIn('u_moonRadiance *', source)
            self.assertIn('kMoonKeyScale * u_moonIllum * SUN_IRRADIANCE_SCALE', source)
            self.assertNotIn('const vec3 kMoonColor', source)
        foliage = (ROOT / 'res/shaders/foliage.frag').read_text(encoding='utf-8')
        self.assertIn('color *= mix(1.0, u_exposure, nightFactor);', foliage)
        source = (ROOT / 'src/luminumbra_client/rendering/passes/FoliagePass.cpp').read_text(encoding='utf-8')
        for field, uniform in (('moon_radiance', 'u_moonRadiance'),
                               ('moon_illumination', 'u_moonIllum'), ('exposure', 'u_exposure')):
            self.assertIn(f'.{field} = glGetUniformLocation(program, "{uniform}")', source)
            self.assertIn(f'm_draw_uniforms.{field}', source)
            self.assertIn(f'ctx.{field}', source)



class UniformCacheWiring(unittest.TestCase):
    def test_location_hit_has_one_container_lookup_and_misses_remain_cached(self):
        source = read(RENDERING / "Shader.cpp")
        lookup = body(source, "GLint Shader::getUniformLocation(")
        # Container access is the work oracle; glGetUniformLocation alone cannot
        # distinguish the old double-probe hit from the retained-iterator hit.
        self.assertEqual(lookup.count("m_uniformLocationCache.find("), 1)
        self.assertNotRegex(lookup, r"m_uniformLocationCache\s*(?:\[|\.at\s*\()")
        self.assertRegex(lookup, r"return\s+\w+->second\s*;")
        self.assertEqual(lookup.count("glGetUniformLocation("), 1)
        self.assertRegex(lookup, r"m_uniformLocationCache\.(?:try_)?emplace\(name, location\)")
        self.assertIn("m_uniformLocationCache.clear();", body(source, "bool Shader::Reload("))
        gbuffer = read(RENDERING / "passes/GBufferPass.cpp")
        for key in ("candidatePath", "basePath", "mesh_info.meshPath"):
            self.assertNotRegex(gbuffer, r"m_(?:skinnedMeshCache|meshCache)\s*\[\s*"
                                + re.escape(key) + r"\s*\]")

    def test_indexed_names_are_built_inside_generation_cache_factories(self):
        for filename in ("LightingPass.cpp", "SsaoPass.cpp"):
            source = read(RENDERING / "passes" / filename)
            # All decimal conversions in these passes must live in a cache
            # factory. This also catches prefixes/suffixes built before setters.
            factories = re.findall(r"\.Get\([^;]*?\[\]\(std::size_t index\)\s*\{(.*?)\}\);",
                                   source, flags=re.S)
            self.assertTrue(factories, filename)
            self.assertEqual(source.count("std::to_string("),
                             sum(factory.count("std::to_string(") for factory in factories), filename)
        lighting = read(RENDERING / "passes/LightingPass.cpp")
        self.assertIn("lighting_shader->ProgramGeneration()", lighting)
        self.assertIn("m_lightning_overlay_shader->ProgramGeneration()", lighting)
        self.assertIn("m_uniform_names->contribution = {};", lighting)
        self.assertIn("m_uniform_names->normal = {};", lighting)
        self.assertIn("m_uniform_names->flashes.Reset();", lighting)
        self.assertIn("m_uniform_names->bolts.Reset();", lighting)
        ssao = read(RENDERING / "passes/SsaoPass.cpp")
        self.assertIn("m_ssao.ssaoShader->ProgramGeneration()", ssao)
        self.assertIn("m_sample_names.Reset();", body(ssao, "void SsaoPass::init_shaders("))


if __name__ == "__main__":
    unittest.main()
