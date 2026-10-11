#include "luminumbra_client/rendering/RenderPipeline.h"

#include <glm/gtc/matrix_transform.hpp>
#include <gtest/gtest.h>

#include <array>
#include <set>

namespace Luminumbra::Rendering {

struct TerrainCullingTestPeer {
    static std::set<ChunkID> Cull(const std::vector<IVec3>& coordinates,
                                  const glm::vec4 (&planes)[6]) {
        std::vector<RenderPipeline::ChunkMeshSnapshot> snapshots;
        for (const auto& coords : coordinates) {
            RenderPipeline::ChunkMeshSnapshot snapshot;
            snapshot.id = Chunk::calculate_id(coords);
            snapshot.coords = coords;
            snapshots.push_back(snapshot);
        }
        RenderPipeline::HierarchicalCuller culler;
        culler.BuildHierarchy(snapshots);
        std::vector<const RenderPipeline::ChunkCullEntry*> visible;
        culler.CullHierarchical(planes, visible);
        std::set<ChunkID> result;
        for (const auto* chunk : visible)
            result.insert(chunk->id);
        return result;
    }
};

} // namespace Luminumbra::Rendering

TEST(TerrainCulling, MatchesWholeChunkBoundsWhenCameraStraddlesPartitionPlanes) {
    using namespace Luminumbra;
    std::vector<IVec3> chunks;
    for (int z = -4; z <= 4; ++z)
        for (int y = 0; y <= 1; ++y)
            for (int x = -4; x <= 4; ++x)
                chunks.emplace_back(x, y, z);

    // The root split is x=z=8, through the centers of the nearest chunks.
    // A loaded fill at (8,17,3) must remain visible from the saved camera pose.
    for (const float offset : {-0.01f, 0.01f, 0.415f}) {
        for (const float yaw : {-90.0f, 0.0f, 90.0f, 180.0f}) {
            SCOPED_TRACE(testing::Message() << "offset=" << offset << " yaw=" << yaw);
            const glm::vec3 camera(8.0f + offset, 17.125f, 8.01f);
            const glm::vec3 forward(std::cos(glm::radians(yaw)), 0.0f, std::sin(glm::radians(yaw)));
            const auto matrix =
                glm::transpose(glm::perspective(glm::radians(45.0f), 2.4f, 0.1f, 3200.0f) *
                               glm::lookAt(camera, camera + forward, glm::vec3(0, 1, 0)));
            const glm::vec4 planes[6] = {matrix[3] + matrix[0],
                                         matrix[3] - matrix[0],
                                         matrix[3] + matrix[1],
                                         matrix[3] - matrix[1],
                                         matrix[3] + matrix[2],
                                         matrix[3] - matrix[2]};
            std::set<ChunkID> expected;
            for (const auto& coords : chunks) {
                bool excluded = false;
                for (const auto& plane : planes) {
                    bool any_corner_inside = false;
                    for (int corner = 0; corner < 8; ++corner) {
                        const glm::vec3 point(
                            static_cast<float>((coords.x + ((corner & 1) != 0)) * CHUNK_SIZE_X),
                            static_cast<float>((coords.y + ((corner & 2) != 0)) * CHUNK_SIZE_Y),
                            static_cast<float>((coords.z + ((corner & 4) != 0)) * CHUNK_SIZE_Z));
                        any_corner_inside |= glm::dot(plane, glm::vec4(point, 1)) >= 0.0f;
                    }
                    excluded |= !any_corner_inside;
                }
                if (!excluded)
                    expected.insert(Chunk::calculate_id(coords));
            }
            const auto visible = Rendering::TerrainCullingTestPeer::Cull(chunks, planes);
            EXPECT_EQ(visible, expected);
            EXPECT_TRUE(visible.contains(Chunk::calculate_id(IVec3(0, 1, 0))));
        }
    }
}
