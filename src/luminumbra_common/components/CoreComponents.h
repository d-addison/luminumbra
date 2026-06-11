#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include <cstdint>
#include <string>
#include <vector>

namespace Luminumbra::Components {

// --- Spatial Component ---

// Represents the position, rotation, and scale of an entity in world space.
// This is the most frequently accessed component. Keeping it small and cohesive
// is critical for cache performance.
struct TransformComponent {
    Vec3 position{0.0f};    // 12 bytes
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // 16 bytes
    Vec3 scale{1.0f};       // 12 bytes
}; // Total size: 40 bytes

// Marker for the entity whose transform represents the active camera.
struct ActiveCameraComponent {};

// --- Identity & Hierarchy ---

// A human-readable name for an entity, primarily for debugging and editor identification.
struct TagComponent {
    std::string tag;
};

// Defines the parent-child relationship between entities, forming a scene graph.
// This allows for complex objects to be composed of multiple entities.
struct HierarchyComponent {
    EntityID parent{entt::null};
    std::vector<EntityID> children;

    // Optimization: A "dirty" flag can be added here later for transform propagation.
    // When a parent's transform changes, it marks its children as dirty,
    // signaling that their world-space transforms need recalculation.
};

struct StaticMeshComponent {
    std::string meshPath;
    // Material LUT id used by the G-Buffer instanced draw (T-I3-16: was
    // hardcoded to 3/grass inside instanced_mesh.vert). Defaults keep the
    // pre-fix appearance for content that never sets it.
    std::uint32_t materialId = 3;
};

// Skinned mesh rendered by the non-instanced skinned G-Buffer stage
// (T-I3-16). The entity must also carry an animation player component
// (luminumbra::animation::AnimationPlayerComponent) whose joint palette the
// renderer uploads to the skinning SSBO each frame.
struct SkinnedMeshComponent {
    std::string meshPath; // .lmesh v2 (LMS2)
    std::uint32_t materialId = 1;
};

// --- State & Lifecycle ---

// A simple marker component to signal that an entity is scheduled for destruction
// at the end of the current tick. This avoids iterator invalidation issues
// that arise from destroying entities mid-update.
struct PendingDeletion {};

} // namespace Luminumbra::Components
