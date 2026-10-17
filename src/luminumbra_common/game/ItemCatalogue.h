#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace luminumbra::game {
// Exact item-name bytes; zero is reserved and is refused by the catalogue.
constexpr std::uint32_t ItemTypeId(std::string_view name) {
    std::uint32_t hash = 2166136261u;
    for (const unsigned char byte : name)
        hash = (hash ^ byte) * 16777619u;
    return hash;
}

struct ItemRender {
    std::string model_id;
    std::uint32_t material_id = 0;
};
struct ItemDefinition {
    std::string id;
    std::uint32_t type = 0;
    std::uint32_t despawn_after = 0;
    std::uint32_t payload = 0;
    std::optional<ItemRender> render;
};

class ItemCatalogue {
public:
    static constexpr std::size_t kMaxBytes = 1'048'576;
    static constexpr std::size_t kMaxItems = 4096;
    static constexpr const char* kSchema = "luminumbra.game.items.v1";
    static constexpr const char* kRelativePath = "data/common/items/items.json";
    // Strict, transactional parsing/loading: failure leaves out unchanged.
    static bool parse(std::string_view bytes, ItemCatalogue& out, std::string& error);
    static bool load(const std::filesystem::path& path, ItemCatalogue& out, std::string& error);
    [[nodiscard]] const ItemDefinition* FindByName(std::string_view name) const;
    [[nodiscard]] const ItemDefinition* FindByType(std::uint32_t type) const;
    // The first consumer has no model registry. Present render references refuse admission.
    [[nodiscard]] bool ValidateGameAdmission(std::string& error) const;

private:
    std::map<std::string, ItemDefinition, std::less<>> m_items;
    std::map<std::uint32_t, std::string> m_types;
};
} // namespace luminumbra::game
