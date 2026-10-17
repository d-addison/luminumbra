#pragma once

#include "luminumbra/core/Types.h"
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace Luminumbra::world {
class GameSession;
}
namespace luminumbra::foliage {
struct FarmingController;
}
namespace luminumbra::game {
// Controller must be the admitted session's authoritative farming controller.
std::uint64_t DropFarmingSeed(Luminumbra::world::GameSession& session,
                              foliage::FarmingController& controller,
                              Luminumbra::Vec3 requested_position);
// One horizontal metre ahead of feet, with 0.25 m clearance. Invalid input refuses.
bool FarmingSeedDropPosition(Luminumbra::Vec3 feet,
                             Luminumbra::Vec3 forward,
                             Luminumbra::Vec3& out);
struct FarmDropSeedEdge {
    bool previous = false;
    bool eligible = false;

    // Call on input transitions too, so a whole ineligible interval between frame samples
    // (such as focus loss/regain in one event batch) cannot arm a drop.
    void Invalidate() {
        eligible = false;
    }
};
// Sample every frame. Only two consecutive eligible samples, up then down, fire.
// Eligibility includes gameplay, pause, overlays and focus; entry only records state.
inline bool FarmDropSeedPressed(bool down, bool gameplay, bool allowed, FarmDropSeedEdge& edge) {
    const bool eligible = gameplay && allowed;
    const bool pressed = eligible && edge.eligible && down && !edge.previous;
    edge.previous = down;
    edge.eligible = eligible;
    return pressed;
}
// Absence adopts the caller's existing default; malformed present blocks refuse transactionally.
bool ParseFarmingInventory(std::string_view metadata, int& seeds, std::string& error);
bool LoadFarmingInventory(const std::filesystem::path& save, int& seeds, std::string& error);
} // namespace luminumbra::game
