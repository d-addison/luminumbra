#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <glad/glad.h>

#include "core/Log.h"
#include "core/RuntimeScenarioHarness.h"
#include "core/scenarios/ScenarioCommon.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h" //  species base_color -> creature tint
#include "luminumbra_common/animation/AnimationRuntime.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/Environment.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/CreatureProcgen.h" //  genome -> body-proportion build
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldStreamingState.h"
#include "rendering/Camera.h"
#include "rendering/LightningBolt.h"
#include "rendering/passes/FoliagePass.h"
#include "rendering/passes/ParticlePass.h"
// lockstep transport seam (engine-generic; ILockstepTransport +
// LoopbackTransport + LockstepSession). Named SendFrame/TryReceiveFrame to dodge
// the <windows.h> SendMessage macro (see LockstepSession.h note).
#include "luminumbra_common/net/LockstepSession.h"
//  (AU1): atmosphere audio telemetry. The harness sweeps the replicated
// weather/wind state through the REAL EnvironmentalAudioSystem atmosphere model +
// the AudioPropagationSystem ambience bed and emits the AtmosphereAudio artifact.
// Client-side dressing only -- no world_hash, no visual-gate dependency.
#include "audio/AudioPropagationSystem.h"
#include "audio/EnvironmentalAudioSystem.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace Luminumbra::Client::ScenarioHarness {

// --- skinned_mesh_visual_smoke ---

namespace {

namespace anim = luminumbra::animation;

// Stable storage for the runtime skeleton/clip the spawned entity's
// AnimationPlayerComponent points at (the scenario spawns exactly once).
// AnimationPlayerComponent holds raw pointers into these, so they must outlive
// the spawned entity and must not move. Static storage is the requirement, not
// a convenience.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
anim::Skeleton g_skinned_test_skeleton;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
anim::AnimationClip g_skinned_test_clip;

// Appends an axis-aligned box (24 vertices, 36 indices, per-face normals)
// fully weighted to a single joint.
void AppendSkinnedBox(std::vector<anim::SkinnedVertexData>& vertices,
                      std::vector<uint32_t>& indices,
                      const Luminumbra::Vec3& min,
                      const Luminumbra::Vec3& max,
                      uint8_t joint) {
    struct Face {
        float normal[3];
        // Corner selector per vertex: 0 -> min component, 1 -> max component.
        int corners[4][3];
    };
    static const Face kFaces[6] = {
        {{1, 0, 0}, {{1, 0, 0}, {1, 1, 0}, {1, 1, 1}, {1, 0, 1}}},
        {{-1, 0, 0}, {{0, 0, 1}, {0, 1, 1}, {0, 1, 0}, {0, 0, 0}}},
        {{0, 1, 0}, {{0, 1, 0}, {0, 1, 1}, {1, 1, 1}, {1, 1, 0}}},
        {{0, -1, 0}, {{0, 0, 0}, {1, 0, 0}, {1, 0, 1}, {0, 0, 1}}},
        {{0, 0, 1}, {{1, 0, 1}, {1, 1, 1}, {0, 1, 1}, {0, 0, 1}}},
        {{0, 0, -1}, {{0, 0, 0}, {0, 1, 0}, {1, 1, 0}, {1, 0, 0}}},
    };
    const float mins[3] = {min.x, min.y, min.z};
    const float maxs[3] = {max.x, max.y, max.z};
    for (const Face& face : kFaces) {
        const uint32_t base = static_cast<uint32_t>(vertices.size());
        for (int v = 0; v < 4; ++v) {
            anim::SkinnedVertexData vertex{};
            for (int c = 0; c < 3; ++c) {
                vertex.pos[c] = face.corners[v][c] ? maxs[c] : mins[c];
                vertex.norm[c] = face.normal[c];
            }
            vertex.uv[0] = (v == 1 || v == 2) ? 1.0f : 0.0f;
            vertex.uv[1] = (v >= 2) ? 1.0f : 0.0f;
            vertex.joints[0] = joint;
            vertex.weights[0] = 255;
            vertices.push_back(vertex);
        }
        indices.push_back(base + 0);
        indices.push_back(base + 1);
        indices.push_back(base + 2);
        indices.push_back(base + 0);
        indices.push_back(base + 2);
        indices.push_back(base + 3);
    }
}

bool WriteSkinnedTestAssets(const std::filesystem::path& mesh_path,
                            const std::filesystem::path& clip_path) {
    // Geometry: a static post (joint 0 "root") from y = 0..2 and an arm
    // (joint 1 "arm") hinged at (0, 2, 0) extending +X. The arm joint
    // rotates about Z from 0 to 120 degrees over the 60 s clip, so any two
    // captures several seconds apart show the arm at visibly different
    // angles with no looping-phase coincidence inside a smoke run.
    std::vector<anim::SkinnedVertexData> vertices;
    std::vector<uint32_t> indices;
    AppendSkinnedBox(vertices, indices, {-0.25f, 0.0f, -0.25f}, {0.25f, 2.0f, 0.25f}, 0);
    AppendSkinnedBox(vertices, indices, {0.1f, 1.8f, -0.2f}, {1.9f, 2.2f, 0.2f}, 1);

    anim::SkinnedMeshAsset mesh{};
    mesh.header.vertexCount = static_cast<uint32_t>(vertices.size());
    mesh.header.indexCount = static_cast<uint32_t>(indices.size());
    mesh.header.jointCount = 2;
    mesh.header.boundingSphere[0] = 0.0f;
    mesh.header.boundingSphere[1] = 1.6f;
    mesh.header.boundingSphere[2] = 0.0f;
    mesh.header.boundingSphere[3] = 3.0f;
    mesh.vertices = std::move(vertices);
    mesh.indices = std::move(indices);

    anim::Lms2Joint root{};
    root.nameHash = anim::HashJointName("root");
    root.parentIndex = -1;
    anim::Lms2Joint arm{};
    arm.nameHash = anim::HashJointName("arm");
    arm.parentIndex = 0;
    arm.localTranslation[1] = 2.0f;
    arm.inverseBind[13] = -2.0f; // column-major translate(0, -2, 0)
    mesh.joints = {root, arm};

    {
        std::ofstream out(mesh_path, std::ios::binary);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char*>(&mesh.header), sizeof(mesh.header));
        out.write(
            reinterpret_cast<const char*>(mesh.vertices.data()),
            static_cast<std::streamsize>(mesh.vertices.size() * sizeof(anim::SkinnedVertexData)));
        out.write(reinterpret_cast<const char*>(mesh.indices.data()),
                  static_cast<std::streamsize>(mesh.indices.size() * sizeof(uint32_t)));
        out.write(reinterpret_cast<const char*>(mesh.joints.data()),
                  static_cast<std::streamsize>(mesh.joints.size() * sizeof(anim::Lms2Joint)));
        if (!out)
            return false;
    }

    anim::AnimClipAsset clip{};
    clip.header.duration = 60.0f;
    clip.header.trackCount = 1;
    anim::AnimTrack track{};
    track.header.jointNameHash = anim::HashJointName("arm");
    track.header.targetType = static_cast<uint32_t>(anim::AnimTargetType::Rotation);
    track.header.keyCount = 2;
    track.header.componentCount = 4;
    track.times = {0.0f, 60.0f};
    // Quaternions x, y, z, w: identity -> 120 degrees about Z.
    track.values = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.86602540f, 0.5f};
    clip.tracks = {track};

    {
        std::ofstream out(clip_path, std::ios::binary);
        if (!out)
            return false;
        out.write(reinterpret_cast<const char*>(&clip.header), sizeof(clip.header));
        for (const anim::AnimTrack& t : clip.tracks) {
            out.write(reinterpret_cast<const char*>(&t.header), sizeof(t.header));
            out.write(reinterpret_cast<const char*>(t.times.data()),
                      static_cast<std::streamsize>(t.times.size() * sizeof(float)));
            out.write(reinterpret_cast<const char*>(t.values.data()),
                      static_cast<std::streamsize>(t.values.size() * sizeof(float)));
        }
        if (!out)
            return false;
    }
    return true;
}

// Sky predicate for the diff gate: blue-led bright pixels (sky and drifting
// clouds). The test mesh renders with the warm sand material, which this
// never matches.
bool IsSkinnedSkyPixel(unsigned char r, unsigned char g, unsigned char b) {
    return b > 110 && static_cast<int>(b) > static_cast<int>(r) + 12;
}

// Warm-toned opaque geometry pixel (the rig's sand material renders in the
// same dim olive band as the surrounding terrain under the current tone
// mapping, measured r/g/b ~ 64/60/33). Counts rig AND terrain — recorded as
// supporting evidence only; the enforced visibility signal is
// skinned_draws > 0 plus the non-sky temporal ROI diff (terrain is static,
// so only the animated rig can move non-sky pixels between captures).
bool IsSkinnedMeshLikePixel(unsigned char r, unsigned char g, unsigned char b) {
    return r >= 30 && r <= 150 && static_cast<int>(r) >= static_cast<int>(b) &&
           static_cast<int>(g) >= static_cast<int>(b);
}

} // namespace

//   integration: in-process replication-driven avatar demo.
void ReplicatedAvatarDemo::Setup(const std::vector<Luminumbra::Vec3>& spawn_positions,
                                 double snapshot_hz) {
    auto pair = Luminumbra::Net::MakeLoopbackPair();
    m_server_tp = std::move(pair.first);
    m_client_tp = std::move(pair.second);
    m_server = std::make_unique<Luminumbra::Net::ReplicationServer>();
    m_server->AddClient(/*client_id=*/0, m_server_tp.get());
    m_client =
        std::make_unique<Luminumbra::Net::ReplicationClient>(/*player_id=*/0, m_client_tp.get());
    m_interp = std::make_unique<Luminumbra::Net::SnapshotInterpolator>();
    m_server_pos = spawn_positions;
    m_period = snapshot_hz > 0.0 ? 1.0 / snapshot_hz : 1.0 / 15.0;
    m_accum = 0.0;
    m_tick = 0;
    m_ready = !m_server_pos.empty();
}

std::vector<Luminumbra::Vec3>
ReplicatedAvatarDemo::Update(double dt_seconds, Luminumbra::Systems::SHIELD_WorldSystem* world) {
    if (!m_ready)
        return {};

    // Walk the SERVER-side avatars forward (+Z toward the camera), terrain-grounded,
    // every frame -- this is the authoritative motion the snapshots carry.
    const float walk = static_cast<float>(dt_seconds) * 1.0f; // ~1 m/s
    for (std::size_t i = 0; i < m_server_pos.size(); ++i) {
        m_server_pos[i].z += walk;
        if (world)
            m_server_pos[i].y = world->GetTerrainHeightAt(m_server_pos[i].x, m_server_pos[i].z);
    }

    // Broadcast a snapshot on the snapshot cadence (NOT every frame), so the client
    // must INTERPOLATE between sparse updates -- the real network behaviour.
    m_accum += dt_seconds;
    if (m_accum >= m_period) {
        m_accum = 0.0;
        ++m_tick;
        std::vector<Luminumbra::Net::ReplEntityState> states;
        states.reserve(m_server_pos.size());
        for (std::size_t i = 0; i < m_server_pos.size(); ++i) {
            Luminumbra::Net::ReplEntityState s;
            s.entity_id = static_cast<std::uint32_t>(i);
            s.px_mm = Luminumbra::Net::ReplQuantPos(m_server_pos[i].x);
            s.py_mm = Luminumbra::Net::ReplQuantPos(m_server_pos[i].y);
            s.pz_mm = Luminumbra::Net::ReplQuantPos(m_server_pos[i].z);
            states.push_back(s);
        }
        m_server->BroadcastSnapshot(m_tick, states);
        m_client->PumpInbound();
        m_server->PumpInbound();
        if (m_client->has_snapshot())
            m_interp->Push(m_client->snapshot());
    }

    // Sample the interpolator render-behind (~1.5 snapshots) so motion is smooth
    // between the sparse updates.
    std::vector<Luminumbra::Vec3> out(m_server_pos.size(), Luminumbra::Vec3(0.0f));
    const double render_tick = static_cast<double>(m_interp->newest_tick()) - 1.5;
    const auto sampled = m_interp->Sample(render_tick);
    for (const auto& e : sampled) {
        if (e.entity_id < out.size()) {
            out[e.entity_id] = Luminumbra::Vec3(Luminumbra::Net::ReplDequantPos(e.px_mm),
                                                Luminumbra::Net::ReplDequantPos(e.py_mm),
                                                Luminumbra::Net::ReplDequantPos(e.pz_mm));
        }
    }
    // Before the first snapshot lands, hold the spawn positions.
    if (sampled.empty())
        return m_server_pos;
    return out;
}

SkinnedMeshVisualTarget SpawnSkinnedMeshVisualEntity(Luminumbra::world::GameSession* game_session,
                                                     const std::filesystem::path& artifact_dir,
                                                     const std::filesystem::path& root_dir,
                                                     int avatar_count) {
    SkinnedMeshVisualTarget target;
    if (!game_session || !game_session->GetWorldSystem()) {
        target.failure_reason = "no_world_system";
        return target;
    }
    auto* world_system = game_session->GetWorldSystem();
    const Luminumbra::Vec3 spawn = game_session->GetMetadata().spawnPoint;

    //  avatar showcase: avatar SHOWCASE row. count==1 -> the unchanged single-rig gate
    // framing (every term below reduces to the original at n==1). count>=2 ->
    // a centered row of rigs ("multiple players beside each other") with the
    // camera pulled back + raised to frame the whole spread.
    const int n = std::max(1, avatar_count);
    const float kRowSpacingM = 2.2f;
    const float mesh_x = spawn.x + 5.0f;
    const float mesh_z = spawn.z + 3.0f;
    const float mesh_y = world_system->GetTerrainHeightAt(mesh_x, mesh_z);
    target.mesh_position = {mesh_x, mesh_y, mesh_z};
    target.focus = target.mesh_position + Luminumbra::Vec3(0.4f, 1.9f, 0.0f);

    // Camera: fixed framing ~9 m south of the row centre, slightly above the arm
    // hinge, lifted clear of the local terrain; widened for a multi-rig row.
    const float cam_x = mesh_x;
    const float cam_z = mesh_z + 9.0f + static_cast<float>(n - 1) * 2.0f;
    const float cam_terrain = world_system->GetTerrainHeightAt(cam_x, cam_z);
    const float cam_y =
        std::max(mesh_y + 2.6f + static_cast<float>(n - 1) * 0.5f, cam_terrain + 1.7f);
    target.camera_position = {cam_x, cam_y, cam_z};

    // Choose the avatar mesh/skeleton/clip. count==1 (the GATE) uses the engine's
    // 2-joint  RIG, byte-identical to the original. count>=2 (the manual
    // SHOWCASE) uses the data-named rigged showcase character + its idle clip, so the row
    // reads as real figures, not abstract test rigs.
    anim::Skeleton* use_skeleton = nullptr;
    anim::AnimationClip* use_clip = nullptr;
    std::string use_mesh_path;
    std::string showcase_label = "showcase"; // data-driven row label (multi-rig showcase)

    if (n <= 1) {
        std::error_code ec;
        std::filesystem::create_directories(artifact_dir / "assets", ec);
        const std::filesystem::path mesh_path = artifact_dir / "assets" / "skinned-test-rig.lmesh";
        const std::filesystem::path clip_path = artifact_dir / "assets" / "skinned-test-rig-";
        if (!WriteSkinnedTestAssets(mesh_path, clip_path)) {
            target.failure_reason = "asset_write_failed";
            return target;
        }
        target.mesh_path = mesh_path.string();
        target.clip_path = clip_path.string();
        // Round-trip through the on-disk formats: the same loaders the renderer
        // and the animation runtime consume.
        anim::SkinnedMeshAsset mesh_asset;
        anim::AnimClipAsset clip_asset;
        if (!anim::LoadSkinnedMeshAsset(target.mesh_path, mesh_asset) ||
            !anim::LoadAnimClipAsset(target.clip_path, clip_asset)) {
            target.failure_reason = "asset_reload_failed";
            return target;
        }
        g_skinned_test_skeleton = anim::BuildSkeleton(mesh_asset);
        g_skinned_test_clip = anim::BuildClip(clip_asset);
        use_skeleton = &g_skinned_test_skeleton;
        use_clip = &g_skinned_test_clip;
        use_mesh_path = target.mesh_path;
    } else {
        // SHOWCASE (row count >= 2): the skinned character mesh + idle clip named by game
        // DATA ( -- the engine source carries no game-content nouns; the model paths +
        // row label live in data/common/scenario/skinned_showcase_model.json). Function-local
        // statics persist for the program lifetime (the player components hold pointers into
        // them), same lifetime guarantee as the g_skinned_test_* globals.
        static anim::Skeleton s_showcase_skeleton;
        static anim::AnimationClip s_showcase_clip;
        std::string mesh_rel, clip_rel;
        {
            std::ifstream in(root_dir / "data/common/scenario/skinned_showcase_model.json");
            if (in.is_open()) {
                try {
                    nlohmann::json j;
                    in >> j;
                    mesh_rel = j.value("mesh", std::string{});
                    clip_rel = j.value("clip", std::string{});
                    showcase_label = j.value("label", showcase_label);
                } catch (...) { /* malformed -> handled by the empty-path guard below */
                }
            }
        }
        if (mesh_rel.empty() || clip_rel.empty()) {
            target.failure_reason = "showcase_model_config_missing";
            return target;
        }
        const std::filesystem::path gmesh = root_dir / mesh_rel;
        const std::filesystem::path gclip = root_dir / clip_rel;
        anim::SkinnedMeshAsset mesh_asset;
        anim::AnimClipAsset clip_asset;
        if (!anim::LoadSkinnedMeshAsset(gmesh.string(), mesh_asset) ||
            !anim::LoadAnimClipAsset(gclip.string(), clip_asset)) {
            target.failure_reason = "showcase_asset_load_failed";
            return target;
        }
        s_showcase_skeleton = anim::BuildSkeleton(mesh_asset);
        s_showcase_clip = anim::BuildClip(clip_asset);
        use_skeleton = &s_showcase_skeleton;
        use_clip = &s_showcase_clip;
        use_mesh_path = gmesh.string();
        target.mesh_path = use_mesh_path;
        target.clip_path = gclip.string();
    }

    entt::registry& registry = game_session->GetRegistry();
    // Spawn the row centred on mesh_x along X. Material ids cycle for visible
    // per-player distinction; the animation phase is staggered so the avatars are
    // not in lock-step (reads as separate players). At n==1 this is byte-identical
    // to the original single test rig (offset 0, material 4, ).
    const std::uint32_t kRowMaterials[5] = {4u, 2u, 1u, 3u, 0u};
    //  procedural creatures: load the species registry so the row's per-creature
    // distinction also reads as DISTINCT SPECIES — each entity takes a species base_color
    // tint, cycling the registered species. Degrades to white (no-op) if none load.
    luminumbra::ai::CreatureSpeciesRegistry row_species;
    {
        std::vector<std::string> _sp_errs;
        row_species.LoadFromDirectory(root_dir / "data" / "common" / "creatures" / "species",
                                      _sp_errs);
    }
    for (int i = 0; i < n; ++i) {
        const float rx =
            mesh_x + (static_cast<float>(i) - static_cast<float>(n - 1) * 0.5f) * kRowSpacingM;
        const float ry = world_system->GetTerrainHeightAt(rx, mesh_z);
        const auto entity = registry.create();
        auto& transform = registry.emplace<Luminumbra::Components::TransformComponent>(entity);
        transform.position = Luminumbra::Vec3(rx, ry, mesh_z);
        //  procedural BUILD: a deterministic per-index spread of body proportions so
        // the showcase row reads as DISTINCT silhouettes (tall/stocky/long), not clones.
        {
            luminumbra::creature::CreatureBuildGenome bg;
            bg.height = static_cast<float>((i * 7 + 2) % 10) / 9.0f;
            bg.girth = static_cast<float>((i * 3 + 5) % 10) / 9.0f;
            bg.length = static_cast<float>((i * 5 + 1) % 10) / 9.0f;
            bg.size = 1.0f;
            const luminumbra::creature::CreatureBuild build =
                luminumbra::creature::ComputeCreatureBuild(bg);
            transform.scale = Luminumbra::Vec3(build.scale_x, build.scale_y, build.scale_z);
        }
        auto& mesh_component =
            registry.emplace<Luminumbra::Components::SkinnedMeshComponent>(entity);
        mesh_component.meshPath = use_mesh_path;
        mesh_component.materialId = kRowMaterials[i % 5];
        if (row_species.size() > 0) {
            const auto& sp = row_species.all()[static_cast<std::size_t>(i) % row_species.size()];
            mesh_component.tintR = sp.base_color[0];
            mesh_component.tintG = sp.base_color[1];
            mesh_component.tintB = sp.base_color[2];
        }
        auto& player = registry.emplace<anim::AnimationPlayerComponent>(entity);
        player.skeleton = use_skeleton;
        player.clip = use_clip;
        player.time = static_cast<double>(i) * 0.3; // staggered phase
        player.looping = true;
        target.all_entities.push_back(entity);
        target.spawn_positions.push_back(Luminumbra::Vec3(rx, ry, mesh_z));
        if (i == 0) {
            target.entity = entity; // primary avatar (the gate ROI tracks this one)
        }
    }

    target.spawned = true;
    LUMINUMBRA_CORE_INFO(
        "skinned_mesh_visual_smoke: spawned {} avatar(s) [{}] centred at ({:.1f}, {:.1f}, {:.1f})",
        n,
        (n <= 1 ? "test-rig" : showcase_label.c_str()),
        target.mesh_position.x,
        target.mesh_position.y,
        target.mesh_position.z);
    return target;
}

void ApplySkinnedMeshVisualCamera(Luminumbra::Rendering::Camera* camera,
                                  const SkinnedMeshVisualTarget& target) {
    if (!camera || !target.spawned) {
        return;
    }
    camera->Position = target.camera_position;
    camera->Zoom = 45.0f;
    AimCameraAt(camera, target.focus);
}

double SkinnedMeshVisualAnimationTime(Luminumbra::world::GameSession* game_session,
                                      const SkinnedMeshVisualTarget& target) {
    if (!game_session || !target.spawned) {
        return -1.0;
    }
    entt::registry& registry = game_session->GetRegistry();
    if (!registry.valid(target.entity) ||
        !registry.all_of<anim::AnimationPlayerComponent>(target.entity)) {
        return -1.0;
    }
    return registry.get<anim::AnimationPlayerComponent>(target.entity).time;
}

SkinnedMeshDiffStats AnalyzeSkinnedMeshCaptures(const std::vector<unsigned char>& pixels_a,
                                                const std::vector<unsigned char>& pixels_b,
                                                int width,
                                                int height) {
    SkinnedMeshDiffStats stats;
    stats.width = width;
    stats.height = height;
    const std::size_t expected =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u;
    if (width <= 0 || height <= 0 || pixels_a.size() < expected || pixels_b.size() < expected) {
        return stats;
    }

    // Central ROI around the framed rig; the top band is excluded so open
    // sky never dominates the diff.
    stats.roi_x0 = width / 4;
    stats.roi_x1 = width - width / 4;
    stats.roi_y0 = height / 5;        // from top
    stats.roi_y1 = (height * 9) / 10; // from top

    constexpr int kChangedChannelDelta = 16;
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;
    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        if (y_from_top < stats.roi_y0 || y_from_top >= stats.roi_y1) {
            continue;
        }
        for (int x = stats.roi_x0; x < stats.roi_x1; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const unsigned char ra = pixels_a[offset + 0u];
            const unsigned char ga = pixels_a[offset + 1u];
            const unsigned char ba = pixels_a[offset + 2u];
            const unsigned char rb = pixels_b[offset + 0u];
            const unsigned char gb = pixels_b[offset + 1u];
            const unsigned char bb = pixels_b[offset + 2u];
            ++stats.roi_pixels;
            if (IsSkinnedMeshLikePixel(ra, ga, ba)) {
                ++stats.mesh_like_pixels_a;
            }
            if (IsSkinnedMeshLikePixel(rb, gb, bb)) {
                ++stats.mesh_like_pixels_b;
            }
            const int delta = std::max({std::abs(static_cast<int>(ra) - static_cast<int>(rb)),
                                        std::abs(static_cast<int>(ga) - static_cast<int>(gb)),
                                        std::abs(static_cast<int>(ba) - static_cast<int>(bb))});
            if (delta >= kChangedChannelDelta &&
                !(IsSkinnedSkyPixel(ra, ga, ba) && IsSkinnedSkyPixel(rb, gb, bb))) {
                ++stats.changed_pixels;
            }
        }
    }
    if (stats.roi_pixels > 0) {
        stats.changed_ratio =
            static_cast<double>(stats.changed_pixels) / static_cast<double>(stats.roi_pixels);
    }

    //  textured-response: spatial color variance across the GREENEST
    // mesh pixels in a tight central sub-ROI of capture A. The creature is
    // framed centrally; restricting to a central box and to green-dominant
    // (creature body) pixels isolates the creature from the warm terrain band so
    // the authored texture's banding/spots drive the variance, while a flat-
    // colored creature would read near-uniform. Two passes (mean, then variance).
    const int cx0 = (stats.roi_x0 + stats.roi_x1) * 3 / 8;
    const int cx1 = (stats.roi_x0 + stats.roi_x1) * 5 / 8;
    const int cy0_top = stats.roi_y0 + (stats.roi_y1 - stats.roi_y0) / 5;
    const int cy1_top = stats.roi_y0 + (stats.roi_y1 - stats.roi_y0) * 4 / 5;
    auto is_creature_px = [](unsigned char r, unsigned char g, unsigned char b) {
        // Green-dominant body pixels (mossy creature), excluding sky/terrain.
        return g > 40 && g >= r && static_cast<int>(g) - static_cast<int>(b) > 8;
    };
    double sum_r = 0, sum_g = 0, sum_b = 0;
    std::uint64_t mesh_n = 0;
    for (int y = 0; y < height; ++y) {
        const int y_from_top = height - 1 - y;
        if (y_from_top < cy0_top || y_from_top >= cy1_top)
            continue;
        for (int x = cx0; x < cx1; ++x) {
            const std::size_t offset =
                static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
            const unsigned char ra = pixels_a[offset + 0u];
            const unsigned char ga = pixels_a[offset + 1u];
            const unsigned char ba = pixels_a[offset + 2u];
            if (!is_creature_px(ra, ga, ba))
                continue;
            sum_r += ra;
            sum_g += ga;
            sum_b += ba;
            ++mesh_n;
        }
    }
    if (mesh_n > 16) {
        const double mr = sum_r / mesh_n, mg = sum_g / mesh_n, mb = sum_b / mesh_n;
        double var_r = 0, var_g = 0, var_b = 0;
        for (int y = 0; y < height; ++y) {
            const int y_from_top = height - 1 - y;
            if (y_from_top < cy0_top || y_from_top >= cy1_top)
                continue;
            for (int x = cx0; x < cx1; ++x) {
                const std::size_t offset =
                    static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u;
                const unsigned char ra = pixels_a[offset + 0u];
                const unsigned char ga = pixels_a[offset + 1u];
                const unsigned char ba = pixels_a[offset + 2u];
                if (!is_creature_px(ra, ga, ba))
                    continue;
                var_r += (ra - mr) * (ra - mr);
                var_g += (ga - mg) * (ga - mg);
                var_b += (ba - mb) * (ba - mb);
            }
        }
        stats.mesh_color_stddev_a =
            (std::sqrt(var_r / mesh_n) + std::sqrt(var_g / mesh_n) + std::sqrt(var_b / mesh_n)) /
            3.0;
    }
    return stats;
}

void WriteSkinnedMeshVisualAnalysis(const std::filesystem::path& artifact_dir,
                                    const SkinnedMeshVisualTarget& target,
                                    const SkinnedMeshVisualCapture& capture_a,
                                    const SkinnedMeshVisualCapture& capture_b,
                                    const SkinnedMeshDiffStats& diff) {
    const std::uint64_t kMinChangedPixels =
        static_cast<std::uint64_t>(ScalePinnedArea(500, kCapturePinnedWidth, kCapturePinnedHeight));
    constexpr double kMinChangedRatio = 0.001;
    // the textured creature drives a strong per-channel color
    // variance across its mesh ROI; a flat-colored creature would sit far below
    // this. Calibrated conservatively (authored texture measures ~20-40).
    constexpr double kMinMeshColorStddev = 6.0;

    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();

    std::vector<std::string> failures;
    if (!target.spawned) {
        failures.push_back("spawn_failed:" + target.failure_reason);
    }
    if (capture_a.skinned_draws == 0 || capture_b.skinned_draws == 0) {
        failures.push_back("skinned_draws_zero");
    }
    if (diff.changed_pixels < kMinChangedPixels) {
        failures.push_back("roi_diff_below_min_pixels");
    }
    if (diff.changed_ratio < kMinChangedRatio) {
        failures.push_back("roi_diff_below_min_ratio");
    }
    if (diff.mesh_color_stddev_a < kMinMeshColorStddev) {
        failures.push_back("mesh_not_textured_flat_color");
    }
    if (capture_b.animation_time_seconds >= 0.0 &&
        capture_b.animation_time_seconds <= capture_a.animation_time_seconds) {
        failures.push_back("animation_clock_not_advancing");
    }
    if (gl_debug.errors != 0) {
        failures.push_back("gl_debug_errors");
    }
    const bool passed = failures.empty();

    const auto capture_json = [](const SkinnedMeshVisualCapture& capture) {
        return nlohmann::json{
            {"file", capture.file},
            {"elapsed_seconds", capture.elapsed_seconds},
            {"animation_time_seconds", capture.animation_time_seconds},
            {"skinned_draws", capture.skinned_draws},
            {"skinned_indices_drawn", capture.skinned_indices_drawn},
        };
    };

    const nlohmann::json artifact = {
        {"schema", "luminumbra.skinned_mesh_visual_analysis.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"scenario", "skinned_mesh_visual_smoke"},
        {"rig",
         {
             {"spawned", target.spawned},
             {"mesh_path", target.mesh_path},
             {"clip_path", target.clip_path},
             {"mesh_position", Vec3ToJson(target.mesh_position)},
             {"camera_position", Vec3ToJson(target.camera_position)},
         }},
        {"capture_a", capture_json(capture_a)},
        {"capture_b", capture_json(capture_b)},
        {"roi",
         {
             {"x0", diff.roi_x0},
             {"y0_from_top", diff.roi_y0},
             {"x1", diff.roi_x1},
             {"y1_from_top", diff.roi_y1},
             {"pixels", diff.roi_pixels},
         }},
        {"diff",
         {
             {"changed_pixels", diff.changed_pixels},
             {"changed_ratio", diff.changed_ratio},
             {"mesh_like_pixels_a", diff.mesh_like_pixels_a},
             {"mesh_like_pixels_b", diff.mesh_like_pixels_b},
             {"mesh_color_stddev_a", diff.mesh_color_stddev_a},
         }},
        {"thresholds",
         {
             {"min_changed_pixels", kMinChangedPixels},
             {"min_changed_ratio", kMinChangedRatio},
             {"min_mesh_color_stddev", kMinMeshColorStddev},
         }},
        {"gl_debug",
         {
             {"messages", gl_debug.messages},
             {"errors", gl_debug.errors},
             {"warnings", gl_debug.warnings},
             {"notifications", gl_debug.notifications},
         }},
        {"failures", failures},
        {"passed", passed},
    };

    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    std::ofstream output(artifact_dir / "skinned-mesh-visual-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
