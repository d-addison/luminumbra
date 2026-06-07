#pragma once

#include "../../../include/luminumbra/core/Types.h"
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
};

// --- State & Lifecycle ---

// A simple marker component to signal that an entity is scheduled for destruction
// at the end of the current tick. This avoids iterator invalidation issues
// that arise from destroying entities mid-update.
struct PendingDeletion {};

} // namespace Luminumbra::Components
