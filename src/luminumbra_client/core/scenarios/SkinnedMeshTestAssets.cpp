#include "SkinnedMeshTestAssets.h"

#include "luminumbra_common/animation/SkinnedMeshFormat.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <utility>
#include <vector>

namespace Luminumbra::Client::ScenarioHarness {
namespace {
namespace anim = luminumbra::animation;

// Appends an axis-aligned box (24 vertices, 36 indices, per-face normals)
// fully weighted to a single joint.
void AppendSkinnedBox(std::vector<anim::SkinnedVertexData>& vertices,
                      std::vector<uint32_t>& indices,
                      const std::array<float, 3>& min,
                      const std::array<float, 3>& max,
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
    const float mins[3] = {min[0], min[1], min[2]};
    const float maxs[3] = {max[0], max[1], max[2]};
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

} // namespace

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
    // This fixture predates LANM2 and writes legacy track headers. Keep its
    // original bytes and nlerp motion independent of the importer's version.
    clip.header.version = anim::kLanimLegacyVersion;
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

} // namespace Luminumbra::Client::ScenarioHarness
