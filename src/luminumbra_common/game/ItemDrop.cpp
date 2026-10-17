#include "ItemDrop.h"
#include "StrictItemJson.h"
#include "systems/FarmingSystem.h"
#include "world/GameSession.h"

#include <climits>
#include <cmath>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace luminumbra::game {
namespace {
bool Finite(Luminumbra::Vec3 position) {
    return std::isfinite(position.x) && std::isfinite(position.y) && std::isfinite(position.z);
}
bool ValidDropPosition(Luminumbra::Vec3 position) {
    using Store = Luminumbra::world::GroundObjectStore;
    return Finite(position) &&
           std::abs(std::round(static_cast<double>(position.x) * 1000.0)) <
               Store::kMaxCoordinateMm &&
           std::abs(std::round(static_cast<double>(position.z) * 1000.0)) <
               Store::kMaxCoordinateMm &&
           std::abs(std::round(static_cast<double>(position.y) * 1000.0)) < Store::kMaxVerticalMm;
}
} // namespace
std::uint64_t DropFarmingSeed(Luminumbra::world::GameSession& session,
                              foliage::FarmingController& controller,
                              Luminumbra::Vec3 position) {
    const auto* catalogue = session.GetItemCatalogue();
    if (!session.ItemConsumerEnabled() || !catalogue || !session.GetWorldSystem() ||
        !session.GetWorldOpenError().empty() || &controller != &session.GetFarmingController() ||
        controller.seeds <= 0 || !ValidDropPosition(position))
        return 0;
    const auto* item = catalogue->FindByName("farming-seed");
    if (!item || item->payload != 0)
        return 0;
    const auto id =
        session.SpawnGroundObject(item->type, position, item->despawn_after, item->payload);
    if (id != 0)
        --controller.seeds;
    return id;
}

bool FarmingSeedDropPosition(Luminumbra::Vec3 feet,
                             Luminumbra::Vec3 forward,
                             Luminumbra::Vec3& out) {
    if (!Finite(feet) || !Finite(forward))
        return false;
    const double length =
        std::hypot(static_cast<double>(forward.x), static_cast<double>(forward.z));
    if (!std::isfinite(length) || length == 0)
        return false;
    const Luminumbra::Vec3 candidate{feet.x + static_cast<float>(forward.x / length),
                                     feet.y + 0.25f,
                                     feet.z + static_cast<float>(forward.z / length)};
    if (!Finite(candidate))
        return false;
    out = candidate;
    return true;
}

bool ParseFarmingInventory(std::string_view metadata, int& seeds, std::string& error) {
    try {
        const auto document = detail::ParseUniqueKeys(metadata);
        if (!document.is_object())
            throw std::invalid_argument("metadata must be an object");
        if (document.contains("farmingInventory")) {
            const auto& block = document.at("farmingInventory");
            if (!block.is_object() || block.size() != 2 ||
                !block.at("version").is_number_unsigned() || block.at("version") != 1 ||
                !block.at("seeds").is_number_unsigned() ||
                block.at("seeds").get<std::uint64_t>() > static_cast<std::uint64_t>(INT_MAX))
                throw std::invalid_argument("invalid keys, version or seed count");
            seeds = block.at("seeds").get<int>();
        }
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Corrupt farming inventory: ") + exception.what();
        return false;
    }
}
bool LoadFarmingInventory(const std::filesystem::path& save, int& seeds, std::string& error) {
    try {
        const auto path = save / "world_info.json";
        if (!std::filesystem::is_regular_file(path) ||
            std::filesystem::file_size(path) > ItemCatalogue::kMaxBytes)
            throw std::invalid_argument("unreadable or oversized metadata");
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::invalid_argument("unreadable metadata");
        std::string bytes(ItemCatalogue::kMaxBytes + 1, '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(input.gcount()));
        if (input.bad() || bytes.size() > ItemCatalogue::kMaxBytes)
            throw std::invalid_argument("metadata read failure/limit");
        return ParseFarmingInventory(bytes, seeds, error);
    } catch (const std::exception& exception) {
        error = std::string("Corrupt farming inventory: ") + exception.what();
        return false;
    }
}
} // namespace luminumbra::game
