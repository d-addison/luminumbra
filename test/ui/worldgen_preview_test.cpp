// Create-world live preview diorama tests.
//
// These tests drive the REAL engine pipeline headlessly (hidden 4.5-core GL
// window) through the WorldgenPreview controller: build a bounded candidate
// world, render it into the offscreen FBO via renderPipeline.render_frame, and
// assert (a) it renders without crashing, (b) changing params / weather / tod
// changes the FBO pixels, (c) orbit changes the view, (d) the per-frame render
// holds a budget at the preview FBO size, and (e) rapid param changes collapse
// into ONE debounced, latest-wins rebuild. They use a FIXED TerrainGenParams
// literal rather than default.json, keeping these assertions independent of
// changes to the authored world-generation presets.
//
// GL-required: the cases GTEST_SKIP without a GL context, and the UI gate
// promotes any "SKIPPED" line on this target to a CTest failure,
// so a real gate run must genuinely render these.

#include "gtest/gtest.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <nlohmann/json.hpp>
#include <numeric>
#include <thread>
#include <vector>

#include "../support/SimulationContentFixture.h"
#include "../support/UniqueTempPath.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"
#include "rendering/Camera.h"
#include "rendering/RenderPipeline.h"
#include "world/WorldgenPreview.h"

namespace fs = std::filesystem;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

fs::path SourceRoot() {
    return fs::weakly_canonical(fs::path(LUMINUMBRA_SOURCE_ROOT));
}

class HiddenGlContext {
public:
    HiddenGlContext() {
        if (!glfwInit()) {
            m_error = "glfwInit failed";
            return;
        }
        m_glfw_initialized = true;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        m_window = glfwCreateWindow(900, 700, "worldgen_preview_test", nullptr, nullptr);
        if (!m_window) {
            m_error = "glfwCreateWindow failed";
            return;
        }
        glfwMakeContextCurrent(m_window);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            m_error = "gladLoadGLLoader failed";
            return;
        }
        m_ready = true;
    }
    ~HiddenGlContext() {
        if (m_window)
            glfwDestroyWindow(m_window);
        if (m_glfw_initialized)
            glfwTerminate();
    }
    bool ready() const {
        return m_ready;
    }
    const std::string& error() const {
        return m_error;
    }

private:
    GLFWwindow* m_window = nullptr;
    bool m_glfw_initialized = false;
    bool m_ready = false;
    std::string m_error;
};

// A self-contained FIXED candidate. NOT default.json. Biomes/structures stay
// off so no data tables are needed; shaping on so there is real relief.
Luminumbra::Systems::TerrainGenParams FixedCandidate() {
    Luminumbra::Systems::TerrainGenParams p;
    p.base_frequency = 0.012f;
    p.base_amplitude = 55.0f;
    p.octaves = 5;
    p.persistence = 0.5f;
    p.lacunarity = 2.0f;
    p.height_offset = 8.0f;
    p.caves_enabled = false;
    p.surface_breaks_enabled = false;
    p.island_mask_enabled = false;
    p.biomes_enabled = false;
    p.biome_relief_enabled = false;
    p.cliffs_enabled = false;
    p.rivers_enabled = false;
    p.lakes_enabled = false;
    p.structures_enabled = false;
    p.hydro_enabled = false;

    p.peaks_amplitude = 48.0f;
    p.peaks_frequency = 0.0025f;
    p.domain_warp_amplitude = 18.0f;
    return p;
}

// Read the preview FBO color back as RGBA8.
std::vector<unsigned char> ReadTarget(const Luminumbra::Client::WorldgenPreview& preview) {
    const int w = preview.target_width();
    const int h = preview.target_height();
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4u);
    glBindTexture(GL_TEXTURE_2D, preview.color_texture());
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return px;
}

// Sum of absolute per-byte differences between two equal-size buffers.
std::uint64_t PixelDelta(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b) {
    const std::size_t n = std::min(a.size(), b.size());
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        acc += static_cast<std::uint64_t>(std::abs(int(a[i]) - int(b[i])));
    }
    return acc;
}

// Number of non-black (foreground) pixels — proves the diorama actually drew.
std::size_t Foreground(const std::vector<unsigned char>& px) {
    std::size_t fg = 0;
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
        if (px[i] > 4 || px[i + 1] > 4 || px[i + 2] > 4)
            ++fg;
    }
    return fg;
}

constexpr int kPreviewW = 640;
constexpr int kPreviewH = 480;

// the world BUILD now runs on a background worker thread; tick (past
// the debounce) only SIGNALS it, and render/render_to_backbuffer adopt the
// finished build (pending->live swap + the rebuild_generation bump) on the GL
// thread. So a test drives a rebuild by: tick(past-debounce) to signal, then
// pump render until the generation advances (the worker finished + the swap
// happened). This helper blocks (bounded) on that lifecycle so the rest of the
// assertions stay exactly as before.
inline bool PumpRebuild(Luminumbra::Client::WorldgenPreview& preview,
                        Luminumbra::Rendering::RenderPipeline& pipeline,
                        unsigned expect_generation) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        preview.render(pipeline, 1.0f / 60.0f); // adopts a finished build on the GL thread
        if (preview.rebuild_generation() >= expect_generation && preview.world_ready()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return preview.rebuild_generation() >= expect_generation && preview.world_ready();
}

struct PreviewFixture {
    Luminumbra::Rendering::RenderPipeline pipeline;
    Luminumbra::Client::WorldgenPreview preview;
    bool ok = false;

    explicit PreviewFixture(
        const Luminumbra::Systems::TerrainGenParams& params = FixedCandidate()) {
        ok = pipeline.startup(kPreviewW, kPreviewH, SourceRoot());
        if (!ok)
            return;
        preview.set_active(true);
        preview.ensure_target(kPreviewW, kPreviewH);
        preview.set_params(params, /*seed*/ 4242);
        // Signal the background build (past the debounce), then pump render until
        // the worker finishes and the GL-thread swap lands, so the first render has
        // a world (: the build is asynchronous).
        preview.tick(1.0f);
        PumpRebuild(preview, pipeline, /*expect_generation*/ 1u);
    }
};

struct SavedBindingFixture {
    fs::path root = Luminumbra::TestSupport::CreateUniqueTempDirectory("saved_binding_");
    Luminumbra::JobSystem jobs;
    std::unique_ptr<Luminumbra::world::GameSession> session;
    Luminumbra::Rendering::RenderPipeline pipeline;
    Luminumbra::Rendering::Camera camera{glm::vec3(8, 32, 8)};
    bool ok = false;

    explicit SavedBindingFixture(bool save_initial_edit = true) {
        jobs.startup(2);
        fs::create_directories(root / "worlds" / "atlas" / "presets");
        {
            std::ofstream preset(root / "worlds" / "atlas" / "presets" / "binding.json");
            preset << R"({"schema_rev":6,"generation_params":{"terrain":{
                "base_frequency":0.01,"base_amplitude":0.0,"octaves":1,
                "persistence":0.5,"lacunarity":2.0,"height_offset":8.0,
                "shaping":{"peaks_amplitude":0.0,"domain_warp_amplitude":0.0,
                "continental_spline":[[-1.0,0.0],[1.0,0.0]]},"hydro":{"enabled":false}},
                "features":{"caves_enabled":false}}})";
        }
        Luminumbra::TestSupport::WriteSimulationContentFixture(root);
        session = std::make_unique<Luminumbra::world::GameSession>();
        // Exercise saved rendering independently of regional simulation and rain divergence.
        session->SetActiveRegionsEnabled(false);
        session->SetWeatherRainEnabled(false);
        session->SetJobSystem(&jobs);
        session->SetRootPath(Luminumbra::Filesystem::GenericPathString(root) + "/");
        session->EnableFarWorldPersistenceSafety();
        if (!session->CreateWorld("SavedBinding", "12345", "binding"))
            return;
        if (save_initial_edit) {
            auto edited = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3(4, 0, 4));
            edited->sdf_data.assign(17 * 17 * 17, -3.0f);
            edited->material_data.assign(17 * 17 * 17, 7);
            edited->mark_sdf_loaded_or_edited();
            edited->mark_voxel_data_dirty();
            edited->set_state(Luminumbra::ChunkState::Idle);
            if (!session->GetWorldSystem()->adopt_streamed_chunk(edited) || !session->SaveWorld() ||
                !session->SaveWorldState())
                return;
        }
        const auto id = session->GetMetadata().worldId;
        if ((save_initial_edit && !session->LoadWorld(id)) ||
            !pipeline.startup(64, 64, SourceRoot()))
            return;
        pipeline.attach_farlod_job_system(&jobs);
        pipeline.set_far_world_mode(Luminumbra::Rendering::FarWorldMode::Ladder);
        pipeline.set_far_world_cave_policy(Luminumbra::World::FarCaveMode::BandLimited);
        session->SetBeforeWorldReplacement([this] {
            pipeline.prepare_world_swap();
            session->ClearFarWorldPublication();
        });
        ok = true;
    }
    ~SavedBindingFixture() {
        if (session)
            session->SetBeforeWorldReplacement({});
        pipeline.shutdown();
        session.reset();
        jobs.shutdown();
        std::error_code error;
        fs::remove_all(root, error);
    }
    bool Capture(std::shared_ptr<const Luminumbra::world::FarWorldPublication>& publication,
                 std::string& diagnostic) {
        Luminumbra::world::FarWorldPublicationRequest request;
        if (!pipeline.prepare_saved_far_world_capture(
                session->GetFarWorldSaveRoot(), camera, request)) {
            diagnostic = pipeline.saved_far_world_refusal();
            return false;
        }
        // Poll exactly as production does, retaining the complete computed wanted set.
        for (std::size_t frame = 0; frame <= request.wanted.size(); ++frame) {
            if (session->CaptureFarWorldPublication(request, publication, diagnostic))
                return pipeline.bind_saved_far_world(publication);
            if (!diagnostic.empty()) {
                pipeline.refuse_saved_far_world_publication(diagnostic);
                return false;
            }
        }
        diagnostic = "publication did not finish within its bounded tile steps";
        return false;
    }
};

} // namespace

TEST(SavedFarWorldBinding, SuccessfulLoadBindsAndFailedOpenRetainsTarget) {
    HiddenGlContext context;
    ASSERT_TRUE(context.ready()) << context.error();
    SavedBindingFixture fixture;
    ASSERT_TRUE(fixture.ok);
    EXPECT_FALSE(fixture.pipeline.saved_far_world_renderer().Handoff());
    std::shared_ptr<const Luminumbra::world::FarWorldPublication> publication;
    std::string diagnostic;
    ASSERT_TRUE(fixture.Capture(publication, diagnostic)) << diagnostic;
    const auto* handoff = fixture.pipeline.saved_far_world_renderer().Handoff();
    ASSERT_TRUE(handoff);
    const auto serial = handoff->State().target_serial;
    EXPECT_EQ(serial, publication->publication_serial);
    EXPECT_FALSE(fixture.session->LoadWorld("unavailable_saved_world"));
    EXPECT_EQ(fixture.pipeline.saved_far_world_renderer().Handoff(), handoff);
    EXPECT_EQ(handoff->State().target_serial, serial);
    EXPECT_TRUE(fixture.pipeline.bind_saved_far_world(publication));
    const auto id = fixture.session->GetMetadata().worldId;
    std::size_t checks = 0;
    const Luminumbra::world::GameSession::WorldOpenCheck reject =
        [&checks](const auto&, const auto&, std::uint32_t) {
            ++checks;
            return std::string("saved world identity refused");
        };
    EXPECT_FALSE(fixture.session->LoadWorld(id, reject));
    EXPECT_EQ(checks, 1u);
    EXPECT_EQ(fixture.session->GetWorldOpenError(), "saved world identity refused");
    EXPECT_EQ(fixture.pipeline.saved_far_world_renderer().Handoff(), handoff);
    EXPECT_EQ(handoff->State().target_serial, serial);
    const Luminumbra::World::WorldgenFileStatusReader unavailable = [](const fs::path&,
                                                                       std::error_code& error) {
        error = std::make_error_code(std::errc::permission_denied);
        return fs::file_status(fs::file_type::none);
    };
    EXPECT_FALSE(fixture.session->LoadWorld(id, unavailable));
    EXPECT_EQ(fixture.pipeline.saved_far_world_renderer().Handoff(), handoff);
    EXPECT_EQ(handoff->State().target_serial, serial);
    EXPECT_TRUE(fixture.session->SaveWorld());
}

TEST(SavedFarWorldBinding, FirstStateSaveOfNormalResidencyCapturesReopensAndBinds) {
    HiddenGlContext context;
    ASSERT_TRUE(context.ready()) << context.error();
    SavedBindingFixture fixture(false);
    ASSERT_TRUE(fixture.ok);
    using namespace Luminumbra;
    using Service = Persistence::WorldSaveService;
    auto* world_system = fixture.session->GetWorldSystem();
    ASSERT_TRUE(world_system);
    const auto root = fixture.session->GetWorldSaveDir();
    ASSERT_FALSE(Service::has_chunk_snapshot(root));
    const auto initial = world_system->GetInitialChunkLoadList(fixture.camera.Position);
    ASSERT_GE(initial.size(), 1875u);
    for (const auto& coords : initial) {
        auto chunk = std::make_shared<Chunk>(coords);
        world_system->GenerateChunkData(*chunk, 4);
        chunk->set_state(ChunkState::Idle);
        ASSERT_TRUE(world_system->adopt_streamed_chunk(chunk));
    }
    for (const IVec3 coords : {IVec3(4, 0, 4), IVec3(5, 0, 4)}) {
        const auto chunk = world_system->find_streamed_chunk(coords);
        ASSERT_TRUE(chunk);
        world_system->GenerateChunkData(*chunk);
        chunk->sdf_data[8 + 17 * (8 + 17 * 8)] -= 1.0f;
        chunk->mark_voxel_data_dirty();
    }
    const auto residents = world_system->snapshot_streamed_chunks();
    EXPECT_GT(std::count_if(residents.begin(),
                            residents.end(),
                            [](const auto& chunk) { return chunk && !chunk->needs_authority(); }),
              1800);
    // This is the first state save, not a metadata-only save or a dirty-region rewrite.
    world::WorldStateSaveReport report;
    ASSERT_TRUE(fixture.session->SaveWorldState(&report));
    EXPECT_GE(report.chunks_total, 1875u);
    EXPECT_GE(report.chunks_saved, 1875u);
    EXPECT_TRUE(report.saved);
    EXPECT_EQ(report.chunks_dirty, 2u);
    ASSERT_TRUE(Service::has_chunk_snapshot(root));
    std::size_t disk_records = 0;
    for (const auto& entry : fs::directory_iterator(Service::region_directory(root))) {
        if (entry.path().extension() != ".lmr")
            continue;
        std::vector<Service::ContainerRecord> records;
        ASSERT_TRUE(Service::read_container_records(entry.path(), records));
        disk_records += std::count_if(records.begin(), records.end(), [](const auto& record) {
            return record.lod_level == 0;
        });
    }
    EXPECT_GE(disk_records, 1875u);
    std::shared_ptr<const world::FarWorldPublication> publication;
    std::string diagnostic;
    ASSERT_TRUE(fixture.Capture(publication, diagnostic)) << diagnostic;
    ASSERT_TRUE(publication);
    const auto generation = publication->committed_generation;
    const auto id = fixture.session->GetMetadata().worldId;
    ASSERT_TRUE(fixture.session->LoadWorld(id));
    EXPECT_FALSE(fixture.pipeline.saved_far_world_renderer().Handoff());
    ASSERT_TRUE(fixture.Capture(publication, diagnostic)) << diagnostic;
    ASSERT_TRUE(publication);
    EXPECT_EQ(publication->committed_generation, generation);
    ASSERT_TRUE(fixture.pipeline.saved_far_world_renderer().Handoff());
    EXPECT_EQ(fixture.pipeline.saved_far_world_renderer().Handoff()->State().target_serial,
              publication->publication_serial);
    const auto edited = fixture.session->GetWorldSystem()->find_authoritative_chunk({4, 0, 4});
    ASSERT_TRUE(edited);
    EXPECT_EQ(edited->sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
}

TEST(SavedFarWorldBinding, ProductionWantedSetAndReplacementAdmitWithinUnchangedTierBudgets) {
    HiddenGlContext context;
    ASSERT_TRUE(context.ready()) << context.error();
    SavedBindingFixture fixture;
    ASSERT_TRUE(fixture.ok);
    using namespace Luminumbra;
    auto* const world_system = fixture.session->GetWorldSystem();
    ASSERT_TRUE(world_system);
    // Park real unsaved edits before populating the normal initial-load footprint.
    // Generate their shared boundaries rather than manufacturing conflicting lattices.
    world_system->debug_set_streaming_radius_cap(1);
    std::vector<std::shared_ptr<Chunk>> parked_edits;
    for (const IVec3 coords : {IVec3(40, 0, 4), IVec3(41, 0, 4)}) {
        auto chunk = std::make_shared<Chunk>(coords);
        world_system->GenerateChunkData(*chunk);
        chunk->sdf_data[8 + 17 * (8 + 17 * 8)] -= 1.0f;
        chunk->mark_sdf_loaded_or_edited();
        chunk->mark_voxel_data_dirty();
        chunk->set_state(ChunkState::Idle);
        ASSERT_TRUE(world_system->adopt_streamed_chunk(chunk));
        parked_edits.push_back(std::move(chunk));
    }
    for (int step = 0; step < 12; ++step) {
        world_system->update(fixture.session->GetRegistry(),
                             fixture.camera.Position,
                             fixture.session->GetPhysicsSystem());
        world_system->wait_for_streaming_jobs();
    }
    for (const auto& chunk : parked_edits) {
        EXPECT_FALSE(world_system->find_streamed_chunk(chunk->get_coords()));
        ASSERT_EQ(world_system->find_authoritative_chunk(chunk->get_coords()), chunk);
        EXPECT_TRUE(chunk->is_voxel_data_dirty());
    }
    ASSERT_GE(world_system->parking_status().parked_count, parked_edits.size());
    const auto initial = world_system->GetInitialChunkLoadList(fixture.camera.Position);
    ASSERT_GE(initial.size(), 1875u);
    for (const auto& coords : initial) {
        if (world_system->find_streamed_chunk(coords))
            continue;
        if (world_system->restore_authoritative_chunk(coords)) {
            ASSERT_TRUE(world_system->find_streamed_chunk(coords));
            ASSERT_FALSE(world_system->durable_read_failed());
            continue;
        }
        auto chunk = std::make_shared<Chunk>(coords);
        world_system->GenerateChunkData(*chunk,
                                        4); // The normal coarse, regenerable residency path.
        chunk->set_state(ChunkState::Idle);
        ASSERT_TRUE(world_system->adopt_streamed_chunk(chunk));
    }
    const auto live_edit = world_system->find_streamed_chunk({2, 0, 2});
    ASSERT_TRUE(live_edit);
    world_system->GenerateChunkData(*live_edit);
    live_edit->sdf_data[8 + 17 * (8 + 17 * 8)] -= 1.0f;
    live_edit->mark_sdf_loaded_or_edited();
    live_edit->mark_voxel_data_dirty();
    const auto residents = world_system->snapshot_streamed_chunks();
    ASSERT_GE(residents.size(), 1875u);
    EXPECT_GT(std::count_if(residents.begin(),
                            residents.end(),
                            [](const auto& chunk) { return chunk && !chunk->needs_authority(); }),
              1800);
    std::vector<World::FarTileKey> wanted;
    ASSERT_EQ(
        World::ComputeFarWantedSet(fixture.camera.Position.x, fixture.camera.Position.z, wanted),
        World::FarWantedError::None);
    ASSERT_GE(
        std::count_if(wanted.begin(), wanted.end(), [](const auto& key) { return key.tier == 1; }),
        16);
    world::FarWorldPublicationRequest request;
    ASSERT_TRUE(fixture.pipeline.prepare_saved_far_world_capture(
        fixture.session->GetFarWorldSaveRoot(), fixture.camera, request));
    ASSERT_EQ(std::vector<World::FarTileKey>(request.wanted.begin(), request.wanted.end()), wanted);
    const auto captures = fixture.session->FarWorldCaptureCount();
    std::shared_ptr<const world::FarWorldPublication> publication;
    std::string diagnostic;
    for (std::size_t frame = 0; frame <= wanted.size(); ++frame) {
        const auto observations = fixture.session->FarWorldObservationCaptureCount();
        const bool complete =
            fixture.session->CaptureFarWorldPublication(request, publication, diagnostic);
        ASSERT_TRUE(diagnostic.empty()) << diagnostic;
        EXPECT_LE(fixture.session->FarWorldObservationCaptureCount() - observations, 1u);
        EXPECT_EQ(complete, frame == wanted.size());
    }
    ASSERT_TRUE(publication);
    ASSERT_EQ(publication->tiles.size(), wanted.size());
    for (std::size_t i = 0; i < wanted.size(); ++i)
        EXPECT_EQ(publication->tiles[i].key, wanted[i]);
    EXPECT_EQ(fixture.session->FarWorldCaptureCount(), captures + 1);
    ASSERT_TRUE(fixture.pipeline.bind_saved_far_world(publication));
    const auto old = publication; // Keep old/new overlap charged, like serving workers.
    // Direct edited revisions must still change the token above 1,024 total residents.
    const auto token = world_system->far_world_publication_token();
    live_edit->sdf_data[8 + 17 * (8 + 17 * 8)] -= 1.0f;
    live_edit->mark_voxel_data_dirty();
    EXPECT_NE(world_system->far_world_publication_token(), token);
    ASSERT_TRUE(fixture.session->SaveWorld());
    ASSERT_TRUE(fixture.Capture(publication, diagnostic)) << diagnostic;
    EXPECT_GT(publication->committed_generation, old->committed_generation);
    const auto limits = Rendering::FarWorldDevelopmentLimits();
    for (const auto& pool : request.memory) {
        ASSERT_TRUE(pool);
        EXPECT_EQ(pool->limit(), 256ull * 1024 * 1024);
        EXPECT_LE(pool->peak(), pool->limit());
        EXPECT_LE(pool->used() + limits.build_headroom_bytes, pool->limit());
        EXPECT_EQ(pool->refusals(), 0u);
    }
    EXPECT_EQ(fixture.pipeline.saved_far_world_renderer().Handoff()->State().target_serial,
              publication->publication_serial);
}

TEST(SavedFarWorldBinding, LoadingCompletionRetriesOnceAndBindsWithoutPublicationInputChanges) {
    HiddenGlContext context;
    ASSERT_TRUE(context.ready()) << context.error();
    SavedBindingFixture fixture;
    ASSERT_TRUE(fixture.ok);
    using namespace Luminumbra;
    auto* const world_system = fixture.session->GetWorldSystem();
    ASSERT_TRUE(world_system);
    auto loading = std::make_shared<Chunk>(IVec3(1, 0, 1));
    world_system->GenerateChunkData(*loading, 4);
    loading->set_state(ChunkState::Idle);
    ASSERT_TRUE(world_system->adopt_streamed_chunk(loading));
    loading->set_state(ChunkState::Loading);
    ASSERT_FALSE(loading->needs_authority());
    const auto before_token = world_system->far_world_publication_token();
    world::FarWorldPublicationRequest request;
    ASSERT_TRUE(fixture.pipeline.prepare_saved_far_world_capture(
        fixture.session->GetFarWorldSaveRoot(), fixture.camera, request));
    const auto captures = fixture.session->FarWorldCaptureCount();
    std::shared_ptr<const world::FarWorldPublication> publication;
    std::string diagnostic;
    EXPECT_FALSE(fixture.session->CaptureFarWorldPublication(request, publication, diagnostic));
    EXPECT_NE(diagnostic.find("generation publication is pending"), std::string::npos);
    EXPECT_FALSE(publication);
    EXPECT_EQ(fixture.session->FarWorldCaptureCount(), captures + 1);
    for (int frame = 0; frame < 120; ++frame) {
        EXPECT_FALSE(fixture.session->CaptureFarWorldPublication(request, publication, diagnostic));
        EXPECT_TRUE(diagnostic.empty());
    }
    EXPECT_EQ(fixture.session->FarWorldCaptureCount(), captures + 1);
    // The owner publishes generation completion, without moving the camera, changing wanted
    // keys, touching edited revisions, or invalidating the session publication explicitly.
    loading->set_state(ChunkState::Idle);
    const auto after_token = world_system->far_world_publication_token();
    EXPECT_EQ(before_token[0], after_token[0]);
    EXPECT_GT(after_token[1], before_token[1]);
    EXPECT_EQ(before_token[2], after_token[2]);
    EXPECT_EQ(before_token[3], after_token[3]);
    EXPECT_GT(before_token[4], 0u);
    EXPECT_EQ(after_token[4], 0u);
    for (std::size_t frame = 0; frame <= request.wanted.size(); ++frame) {
        const auto observations = fixture.session->FarWorldObservationCaptureCount();
        const bool complete =
            fixture.session->CaptureFarWorldPublication(request, publication, diagnostic);
        ASSERT_TRUE(diagnostic.empty()) << diagnostic;
        EXPECT_LE(fixture.session->FarWorldObservationCaptureCount() - observations, 1u);
        EXPECT_EQ(complete, frame == request.wanted.size());
    }
    EXPECT_EQ(fixture.session->FarWorldCaptureCount(), captures + 2);
    ASSERT_TRUE(publication);
    ASSERT_TRUE(fixture.pipeline.bind_saved_far_world(publication));
    ASSERT_TRUE(fixture.pipeline.saved_far_world_renderer().Handoff());
    EXPECT_EQ(fixture.pipeline.saved_far_world_renderer().Handoff()->State().target_serial,
              publication->publication_serial);
    const auto attempts = request.source_memory->attempts();
    for (int frame = 0; frame < 120; ++frame) {
        EXPECT_FALSE(fixture.session->CaptureFarWorldPublication(request, publication, diagnostic));
        EXPECT_TRUE(diagnostic.empty());
    }
    EXPECT_EQ(fixture.session->FarWorldCaptureCount(), captures + 2);
    EXPECT_EQ(request.source_memory->attempts(), attempts);
}

TEST(SavedFarWorldBinding, CaveSaveAsAndWorldReplacementDrainOldTargets) {
    HiddenGlContext context;
    ASSERT_TRUE(context.ready()) << context.error();
    SavedBindingFixture fixture;
    ASSERT_TRUE(fixture.ok);
    std::shared_ptr<const Luminumbra::world::FarWorldPublication> publication;
    std::string diagnostic;
    ASSERT_TRUE(fixture.Capture(publication, diagnostic)) << diagnostic;
    const auto original = publication;
    ASSERT_TRUE(fixture.pipeline.saved_far_world_renderer().Handoff());
    // Admit work before each invalidation, exercising the renderer's drain seam.
    // The worker owns only frozen observations even while the session is replaced.
    auto advance = [&fixture] {
        fixture.pipeline.render_frame(fixture.session->GetRegistry(),
                                      *fixture.session->GetWorldSystem(),
                                      fixture.camera,
                                      0,
                                      false);
        EXPECT_EQ(fixture.pipeline.far_world_selection().effective,
                  Luminumbra::Rendering::FarWorldMode::Ladder);
    };
    advance();
    fixture.pipeline.set_far_world_cave_policy(Luminumbra::World::FarCaveMode::BoxFiltered);
    fixture.session->ClearFarWorldPublication();
    EXPECT_FALSE(fixture.pipeline.saved_far_world_renderer().Handoff());
    EXPECT_EQ(fixture.pipeline.prev_view_proj(), glm::mat4(1));
    EXPECT_FALSE(fixture.pipeline.bind_saved_far_world(original));
    ASSERT_TRUE(fixture.Capture(publication, diagnostic)) << diagnostic;
    EXPECT_GT(publication->world_epoch, original->world_epoch);
    advance();
    const auto before_save_as = publication;
    const auto copy = fixture.root / "copy";
    // Persistence safety must cover save-as even when rendering has no save policy installed.
    fixture.session->SetFarEditedCachePolicy({});
    ASSERT_FALSE(fixture.session->FarEditedCacheEnabled());
    const auto source = fixture.session->GetWorldSaveDir();
    Luminumbra::Persistence::SnapshotIndex committed;
    ASSERT_TRUE(Luminumbra::Persistence::read_snapshot_index(source, committed, nullptr));
    EXPECT_FALSE(fixture.session->RunInSnapshotTransaction([&] {
        EXPECT_NE(Luminumbra::Persistence::SaveTransaction::active_for(source), nullptr);
        EXPECT_TRUE(fixture.session->SaveWorld());
        return false;
    }));
    Luminumbra::Persistence::SnapshotIndex rolled_back;
    ASSERT_TRUE(Luminumbra::Persistence::read_snapshot_index(source, rolled_back, nullptr));
    EXPECT_EQ(rolled_back.generation, committed.generation);
    ASSERT_TRUE(fixture.pipeline.saved_far_world_renderer().Handoff());
    EXPECT_EQ(fixture.pipeline.saved_far_world_renderer().Handoff()->State().target_serial,
              before_save_as->publication_serial);
    ASSERT_TRUE(fixture.session->SaveWorldStateTo(copy));
    EXPECT_TRUE(fs::is_regular_file(copy / "world_info.json"));
    EXPECT_FALSE(fixture.pipeline.saved_far_world_renderer().Handoff());
    EXPECT_EQ(fixture.pipeline.prev_view_proj(), glm::mat4(1));
    EXPECT_FALSE(fixture.pipeline.bind_saved_far_world(before_save_as));
    ASSERT_TRUE(fixture.Capture(publication, diagnostic)) << diagnostic;
    EXPECT_EQ(publication->save_root, copy);
    advance();
    const auto before_swap = publication;
    const auto id = fixture.session->GetMetadata().worldId;
    ASSERT_TRUE(fixture.session->LoadWorld(id));
    EXPECT_FALSE(fixture.pipeline.saved_far_world_renderer().Handoff());
    EXPECT_EQ(fixture.pipeline.prev_view_proj(), glm::mat4(1));
    EXPECT_FALSE(fixture.pipeline.bind_saved_far_world(before_swap));
    ASSERT_TRUE(fixture.Capture(publication, diagnostic)) << diagnostic;
    fixture.pipeline.prepare_world_swap();
    fixture.session->ClearFarWorldPublication();
    EXPECT_FALSE(fixture.pipeline.saved_far_world_renderer().Handoff());
    EXPECT_FALSE(fixture.pipeline.bind_saved_far_world(publication));
}

TEST(SavedFarWorldBinding, UnavailableSavedEditRetainsTargetWithoutFixtureSubstitution) {
    HiddenGlContext context;
    ASSERT_TRUE(context.ready()) << context.error();
    SavedBindingFixture fixture;
    ASSERT_TRUE(fixture.ok);
    std::shared_ptr<const Luminumbra::world::FarWorldPublication> publication;
    std::string diagnostic;
    ASSERT_TRUE(fixture.Capture(publication, diagnostic)) << diagnostic;
    const auto* handoff = fixture.pipeline.saved_far_world_renderer().Handoff();
    ASSERT_TRUE(handoff);
    const auto serial = handoff->State().target_serial;
    const auto edited = fixture.session->GetWorldSystem()->find_authoritative_chunk({4, 0, 4});
    ASSERT_TRUE(edited);
    ASSERT_FALSE(edited->sdf_data.empty());
    edited->sdf_data.pop_back();
    edited->mark_voxel_data_dirty();
    EXPECT_FALSE(fixture.Capture(publication, diagnostic));
    EXPECT_FALSE(fixture.pipeline.saved_far_world_refusal().empty());
    EXPECT_EQ(fixture.pipeline.saved_far_world_renderer().Handoff(), handoff);
    EXPECT_EQ(handoff->State().target_serial, serial);
    EXPECT_EQ(fixture.pipeline.far_world_renderer()
                  .Selection(Luminumbra::Rendering::FarWorldPurpose::World)
                  .refusal,
              Luminumbra::Rendering::FarWorldRefusal::ImmutableAuthorityUnavailable);
}

TEST(SavedFarWorldBinding, NoBindingPreservesFixtureDiagnosticsAndPreviewSelection) {
    HiddenGlContext context;
    ASSERT_TRUE(context.ready()) << context.error();
    SavedBindingFixture fixture;
    ASSERT_TRUE(fixture.ok);
    using namespace Luminumbra::Rendering;
    const auto original = FarWorldDiagnosticJson(fixture.pipeline.far_world_diagnostic());
    const auto selection = fixture.pipeline.far_world_renderer().Selection(FarWorldPurpose::World);
    EXPECT_EQ(selection.refusal, FarWorldRefusal::ImmutableAuthorityUnavailable);
    EXPECT_FALSE(fixture.pipeline.bind_saved_far_world({}));
    EXPECT_FALSE(fixture.pipeline.saved_far_world_refusal().empty());
    EXPECT_FALSE(fixture.pipeline.saved_far_world_renderer().Handoff());
    fixture.pipeline.set_far_lod_preview_anchor(glm::vec3(0), 176);
    Luminumbra::world::FarWorldPublicationRequest request;
    EXPECT_FALSE(fixture.pipeline.prepare_saved_far_world_capture(
        fixture.session->GetFarWorldSaveRoot(), fixture.camera, request));
    EXPECT_EQ(fixture.pipeline.far_world_renderer().Selection(FarWorldPurpose::Preview).refusal,
              FarWorldRefusal::PreviewOverride);
    fixture.pipeline.clear_far_lod_preview_anchor();
    EXPECT_EQ(FarWorldDiagnosticJson(fixture.pipeline.far_world_diagnostic()), original);
}

TEST(SavedFarWorldBinding, RenderOnAndOffLeaveSimulationAndSchedulerEqual) {
    HiddenGlContext context;
    ASSERT_TRUE(context.ready()) << context.error();
    SavedBindingFixture fixture;
    ASSERT_TRUE(fixture.ok);
    const auto hash = fixture.session->ComputeChunkStateHash();
    const auto water = fixture.session->GetWorldSystem()->debug_water_state_hash();
    const auto schedule = fixture.session->GetRegionSchedule();
    const auto tick = fixture.session->GetSimulationTickCount();
    std::shared_ptr<const Luminumbra::world::FarWorldPublication> publication;
    std::string diagnostic;
    ASSERT_TRUE(fixture.Capture(publication, diagnostic)) << diagnostic;
    for (const auto mode : {Luminumbra::Rendering::FarWorldMode::Ladder,
                            Luminumbra::Rendering::FarWorldMode::Legacy}) {
        fixture.pipeline.set_far_world_mode(mode);
        fixture.pipeline.render_frame(fixture.session->GetRegistry(),
                                      *fixture.session->GetWorldSystem(),
                                      fixture.camera,
                                      0,
                                      false);
        EXPECT_EQ(fixture.session->ComputeChunkStateHash(), hash);
        EXPECT_EQ(fixture.session->GetWorldSystem()->debug_water_state_hash().hash, water.hash);
        EXPECT_EQ(fixture.session->GetRegionSchedule(), schedule);
        EXPECT_EQ(fixture.session->GetSimulationTickCount(), tick);
        EXPECT_FALSE(fixture.session->ActiveRegionsEnabled());
    }
}

TEST(SavedFarWorldBinding, UnuploadedAndStaleTerrainDoNotClaimSeparatelyReadyWater) {
    using namespace Luminumbra::Rendering;
    FarWorldLiveUpload upload;
    upload.snapshot_version = 2;
    upload.uploaded_version = 1;
    upload.uploaded_indices = 3;
    upload.valid_allocation = true;
    upload.water_snapshot_version = 4;
    upload.water_uploaded_version = 4;
    upload.water_uploaded_indices = 3;
    upload.water_valid_allocation = true;
    upload.water_pass_eligible = true;
    EXPECT_FALSE(IsFarWorldLiveUploadCurrent(upload));
    EXPECT_TRUE(IsFarWorldLiveWaterUploadCurrent(upload));
    upload.uploaded_version = 2;
    upload.valid_allocation = false;
    EXPECT_FALSE(IsFarWorldLiveUploadCurrent(upload));
    upload.water_uploaded_version = 3;
    EXPECT_FALSE(IsFarWorldLiveWaterUploadCurrent(upload));
    upload.valid_allocation = true;
    EXPECT_TRUE(IsFarWorldLiveUploadCurrent(upload));
    EXPECT_FALSE(IsFarWorldLiveWaterUploadCurrent(upload));
}

TEST(WorldgenPreviewTest, RendersCandidateDioramaToTargetWithoutCrashing) {
    HiddenGlContext ctx;
    if (!ctx.ready())
        GTEST_SKIP() << ctx.error();

    PreviewFixture fx;
    ASSERT_TRUE(fx.ok) << "RenderPipeline startup failed";
    ASSERT_TRUE(fx.preview.world_ready());

    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> px = ReadTarget(fx.preview);
    // A real lit diorama fills a meaningful fraction of the frame (terrain + sky).
    EXPECT_GT(Foreground(px), static_cast<std::size_t>(kPreviewW * kPreviewH / 20));
    // The offscreen redirect must be cleared after render (default-0 path).
    EXPECT_FALSE(fx.pipeline.has_offscreen_target());
}

TEST(WorldgenPreviewTest, ChangingParamsChangesTargetPixels) {
    HiddenGlContext ctx;
    if (!ctx.ready())
        GTEST_SKIP() << ctx.error();

    PreviewFixture fx;
    ASSERT_TRUE(fx.ok);
    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> before = ReadTarget(fx.preview);

    // A markedly different relief profile (taller, rougher) => different terrain.
    Luminumbra::Systems::TerrainGenParams p = FixedCandidate();
    p.base_amplitude = 120.0f;
    p.peaks_amplitude = 110.0f;
    p.height_offset = 30.0f;
    const unsigned gen0 = fx.preview.rebuild_generation();
    fx.preview.set_params(p, 4242);
    fx.preview.tick(1.0f); // past debounce -> SIGNAL the background rebuild
    // Pump render until the worker finishes + the GL-thread swap bumps the gen.
    ASSERT_TRUE(PumpRebuild(fx.preview, fx.pipeline, gen0 + 1u));
    EXPECT_EQ(fx.preview.rebuild_generation(), gen0 + 1u);
    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> after = ReadTarget(fx.preview);

    EXPECT_GT(PixelDelta(before, after), static_cast<std::uint64_t>(kPreviewW * kPreviewH));
}

TEST(WorldgenPreviewTest, ChangingWeatherAndTimeOfDayChangesTargetPixels) {
    HiddenGlContext ctx;
    if (!ctx.ready())
        GTEST_SKIP() << ctx.error();

    PreviewFixture fx;
    ASSERT_TRUE(fx.ok);

    fx.preview.set_time_of_day(0.05f); // near-noon, bright
    fx.preview.set_weather(Luminumbra::Client::WorldgenPreview::Weather::Clear);
    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> noon = ReadTarget(fx.preview);

    fx.preview.set_time_of_day(0.24f); // golden dusk
    fx.preview.set_weather(Luminumbra::Client::WorldgenPreview::Weather::Storm);
    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> dusk_storm = ReadTarget(fx.preview);

    EXPECT_GT(PixelDelta(noon, dusk_storm), static_cast<std::uint64_t>(kPreviewW * kPreviewH));
}

TEST(WorldgenPreviewTest, OrbitChangesTheView) {
    HiddenGlContext ctx;
    if (!ctx.ready())
        GTEST_SKIP() << ctx.error();

    PreviewFixture fx;
    ASSERT_TRUE(fx.ok);

    fx.preview.reset_view();
    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> a = ReadTarget(fx.preview);
    const float yaw0 = fx.preview.orbit_yaw();

    fx.preview.orbit(/*dyaw*/ 70.0f, /*dpitch*/ 12.0f);
    EXPECT_NE(fx.preview.orbit_yaw(), yaw0);
    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> b = ReadTarget(fx.preview);

    EXPECT_GT(PixelDelta(a, b), static_cast<std::uint64_t>(kPreviewW * kPreviewH));
}

TEST(WorldgenPreviewTest, RebuildIsDebouncedAndLatestWins) {
    HiddenGlContext ctx;
    if (!ctx.ready())
        GTEST_SKIP() << ctx.error();

    PreviewFixture fx;
    ASSERT_TRUE(fx.ok);
    const unsigned gen0 = fx.preview.rebuild_generation();

    // 5 rapid param changes, each within the debounce window. Tiny dt ticks keep
    // re-arming the timer, so NONE of them rebuild yet.
    for (int i = 0; i < 5; ++i) {
        Luminumbra::Systems::TerrainGenParams p = FixedCandidate();
        p.base_amplitude = 40.0f + static_cast<float>(i) * 12.0f;
        fx.preview.set_params(p, 4242);
        fx.preview.tick(0.01f); // well under the 0.25 s debounce
    }
    EXPECT_EQ(fx.preview.rebuild_generation(), gen0) << "no rebuild should fire mid-burst";

    // Let the debounce window elapse -> the burst collapses to ONE signalled build.
    // Pump render until the worker finishes + the GL-thread swap bumps the gen
    // (: the actual rebuild + generation bump are asynchronous).
    fx.preview.tick(1.0f);
    ASSERT_TRUE(PumpRebuild(fx.preview, fx.pipeline, gen0 + 1u));
    EXPECT_EQ(fx.preview.rebuild_generation(), gen0 + 1u) << "exactly one rebuild for the burst";
    // And no further rebuild once settled (dirty was cleared at signal time).
    fx.preview.tick(1.0f);
    fx.preview.render(fx.pipeline, 1.0f / 60.0f);
    EXPECT_EQ(fx.preview.rebuild_generation(), gen0 + 1u);
}

TEST(WorldgenPreviewTest, PerFrameRenderProducesComparableObservation) {
    HiddenGlContext ctx;
    if (!ctx.ready())
        GTEST_SKIP() << ctx.error();

    PreviewFixture fx;
    ASSERT_TRUE(fx.ok);
    ASSERT_TRUE(fx.preview.world_ready());

    // Warm the shader/state/pipeline, then record a comparable observation. The
    // preview re-renders only when something changed, so each sampled frame moves
    // the camera and includes render-to-FBO plus the GL flush. Machine-independent
    // regression decisions are made by the paired relative performance gate.
    fx.preview.render(fx.pipeline, 1.0f / 60.0f);
    glFinish();

    constexpr int kFrames = 8;
    double worst_ms = 0.0;
    std::vector<double> samples_ms;
    samples_ms.reserve(kFrames);
    for (int i = 0; i < kFrames; ++i) {
        // Nudge the camera each frame so the render path actually re-runs.
        fx.preview.orbit(3.0f, 0.0f);
        const auto t0 = std::chrono::steady_clock::now();
        fx.preview.render(fx.pipeline, 1.0f / 60.0f);
        glFinish();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        samples_ms.push_back(ms);
        worst_ms = std::max(worst_ms, ms);
    }

    auto sorted_ms = samples_ms;
    std::sort(sorted_ms.begin(), sorted_ms.end());
    const auto percentile = [&sorted_ms](double quantile) {
        const auto rank =
            static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(sorted_ms.size())));
        return sorted_ms[std::min(sorted_ms.size() - 1, std::max<std::size_t>(1, rank) - 1)];
    };
    const double p50_ms = percentile(0.50);
    const double p95_ms = percentile(0.95);
    const double p99_ms = percentile(0.99);
    std::vector<double> deviations;
    deviations.reserve(samples_ms.size());
    for (const double sample_ms : samples_ms) {
        deviations.push_back(std::abs(sample_ms - p50_ms));
    }
    std::sort(deviations.begin(), deviations.end());
    const double mad_ms = deviations[(deviations.size() - 1) / 2];
    const fs::path artifact =
        fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "performance" / "worldgen_preview_frame.json";
    fs::create_directories(artifact.parent_path());
    std::ofstream report(artifact, std::ios::trunc);
    ASSERT_TRUE(report) << "could not write performance evidence: " << artifact;
    report << std::setprecision(17) << "{\n"
           << "  \"schema\": \"luminumbra.performance_measurement.v3\",\n"
           << "  \"status\": \"evaluated\",\n"
           << "  \"enforcement\": \"observation\",\n"
           << "  \"test\": \"WorldgenPreviewTest.PerFrameRenderProducesComparableObservation\",\n"
           << "  \"metric\": \"preview_frame_wall_ms\",\n"
           << "  \"unit\": \"ms\",\n"
           << "  \"direction\": \"lower\",\n"
           << "  \"sample_count\": " << samples_ms.size() << ",\n"
           << "  \"samples\": [";
    for (std::size_t i = 0; i < samples_ms.size(); ++i) {
        if (i > 0)
            report << ", ";
        report << samples_ms[i];
    }
    report << "],\n"
           << "  \"p50\": " << p50_ms << ",\n"
           << "  \"p95\": " << p95_ms << ",\n"
           << "  \"p99\": " << p99_ms << ",\n"
           << "  \"mad\": " << mad_ms << ",\n"
           << "  \"worst\": " << worst_ms << "\n"
           << "}\n";
    report.close();
    ASSERT_FALSE(report.fail()) << "could not finalize performance evidence: " << artifact;
}

TEST(WorldgenPreviewTest, ResolvedShippingHillsRendersWithDefaultTemporalPresentation) {
    HiddenGlContext ctx;
    ASSERT_TRUE(ctx.ready()) << ctx.error();
    const auto preset =
        Luminumbra::world::LoadTerrainPreset(SourceRoot() / "worlds/atlas/presets/default.json");
    ASSERT_TRUE(preset.ok);
    EXPECT_TRUE(preset.params.island_mask_enabled);
    EXPECT_TRUE(preset.params.biomes_enabled);
    EXPECT_TRUE(preset.params.biome_relief_enabled);
    EXPECT_TRUE(preset.params.hydro_enabled);
    EXPECT_TRUE(preset.params.structures_enabled);
    EXPECT_TRUE(preset.params.surface_breaks_enabled);
    EXPECT_TRUE(preset.params.rivers_enabled);
    EXPECT_TRUE(preset.params.lakes_enabled);
    EXPECT_TRUE(preset.params.cliffs_enabled);
    PreviewFixture fx(preset.params);
    ASSERT_TRUE(fx.ok);
    ASSERT_TRUE(fx.preview.world_ready());
    EXPECT_TRUE(fx.pipeline.taau_enabled());
    EXPECT_EQ(fx.pipeline.volumetric_quality(), 1);
    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const auto pixels = ReadTarget(fx.preview);
    EXPECT_GT(Foreground(pixels), static_cast<std::size_t>(kPreviewW * kPreviewH / 20));
    const auto artifact = fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "performance/shipping_hills.ppm";
    fs::create_directories(artifact.parent_path());
    std::ofstream capture(artifact, std::ios::binary);
    ASSERT_TRUE(capture);
    capture << "P6\n" << kPreviewW << ' ' << kPreviewH << "\n255\n";
    for (std::size_t i = 0; i < pixels.size(); i += 4)
        capture.write(reinterpret_cast<const char*>(pixels.data() + i), 3);
    capture.close();
    ASSERT_TRUE(capture);
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}
