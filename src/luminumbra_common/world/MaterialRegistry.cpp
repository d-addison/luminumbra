#include "MaterialRegistry.h"

namespace Luminumbra::World {
namespace {

bool IsCustomMaterialName(std::string_view name) {
    constexpr std::size_t kMaxNameBytes = 128;
    if (name.empty() || name.size() > kMaxNameBytes) {
        return false;
    }
    const std::size_t separator = name.find(':');
    if (separator == std::string_view::npos || separator == 0 || separator + 1 == name.size()) {
        return false;
    }
    for (std::size_t index = 0; index < name.size(); ++index) {
        const char character = name[index];
        if (index != separator && !(character >= 'a' && character <= 'z') &&
            !(character >= '0' && character <= '9') && character != '_' && character != '-' &&
            character != '.') {
            return false;
        }
    }
    return true;
}

} // namespace

MaterialRegistry::MaterialRegistry() {
    m_names[static_cast<u8>(MaterialType::Air)] = "Air";
    m_names[static_cast<u8>(MaterialType::Stone)] = "Stone";
    m_names[static_cast<u8>(MaterialType::Soil)] = "Soil";
    m_names[static_cast<u8>(MaterialType::Grass)] = "Grass";
    m_names[static_cast<u8>(MaterialType::Sand)] = "Sand";
    m_names[static_cast<u8>(MaterialType::Deepslate)] = "Deepslate";
    m_names[static_cast<u8>(MaterialType::LuminCrystal)] = "LuminCrystal";
    m_names[static_cast<u8>(MaterialType::Water)] = "Water";
}

bool MaterialRegistry::Register(std::string_view name, u32 id) {
    if (id < 8 || id >= m_names.size() || !IsCustomMaterialName(name) || !m_names[id].empty() ||
        Resolve(name).has_value()) {
        return false;
    }
    m_names[id] = name;
    return true;
}

std::optional<u8> MaterialRegistry::Resolve(std::string_view name) const {
    if (name.empty() || name.size() > 128) {
        return std::nullopt;
    }
    for (std::size_t index = 0; index < m_names.size(); ++index) {
        if (m_names[index] == name) {
            return static_cast<u8>(index);
        }
    }
    return std::nullopt;
}

} // namespace Luminumbra::World
