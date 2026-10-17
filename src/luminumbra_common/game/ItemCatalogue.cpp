#include "ItemCatalogue.h"
#include "StrictItemJson.h"

#include <fstream>
#include <limits>
#include <stdexcept>

namespace luminumbra::game {
namespace {
using Json = nlohmann::json;
bool NameValid(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() == '-' || name.back() == '-')
        return false;
    bool hyphen = false;
    for (const unsigned char c : name) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') || (c == '-' && hyphen))
            return false;
        hyphen = c == '-';
    }
    return true;
}
std::uint32_t U32(const Json& value) {
    if (!value.is_number_unsigned() || value.get<std::uint64_t>() > UINT32_MAX)
        throw std::invalid_argument("expected strict u32");
    return value.get<std::uint32_t>();
}
} // namespace

bool ItemCatalogue::parse(std::string_view bytes, ItemCatalogue& out, std::string& error) {
    try {
        if (bytes.empty() || bytes.size() > kMaxBytes || bytes.back() != '\n' ||
            bytes.find('\r') != std::string_view::npos || bytes.starts_with("\xef\xbb\xbf"))
            throw std::invalid_argument("expected bounded UTF-8, LF, no BOM and final LF");
        // dump() also validates UTF-8 in parsed string values.
        const auto document = detail::ParseUniqueKeys(bytes);
        (void)document.dump();
        if (!document.is_object() || document.size() != 2 || document.at("schema") != kSchema ||
            !document.at("items").is_array() || document.at("items").size() > kMaxItems)
            throw std::invalid_argument("invalid catalogue root/schema/limit");
        ItemCatalogue candidate;
        std::string previous;
        for (const auto& entry : document.at("items")) {
            if (!entry.is_object() || entry.size() != (entry.contains("render") ? 4u : 3u))
                throw std::invalid_argument("invalid item keys");
            ItemDefinition item;
            item.id = entry.at("id").get<std::string>();
            if (!NameValid(item.id) || (!previous.empty() && item.id <= previous))
                throw std::invalid_argument("invalid, duplicate or unsorted item id");
            previous = item.id;
            item.type = ItemTypeId(item.id);
            item.despawn_after = U32(entry.at("despawn_after"));
            item.payload = U32(entry.at("payload"));
            if (item.type == 0 || candidate.m_types.contains(item.type))
                throw std::invalid_argument("zero or colliding item type");
            if (entry.contains("render")) {
                const auto& render = entry.at("render");
                if (!render.is_object() || render.size() != 2)
                    throw std::invalid_argument("invalid render keys");
                ItemRender reference{render.at("model_id").get<std::string>(),
                                     U32(render.at("material_id"))};
                if (!NameValid(reference.model_id))
                    throw std::invalid_argument("invalid model id");
                item.render = std::move(reference);
            }
            candidate.m_types.emplace(item.type, item.id);
            candidate.m_items.emplace(item.id, std::move(item));
        }
        out = std::move(candidate);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = std::string("Item catalogue: ") + exception.what();
        return false;
    }
}

bool ItemCatalogue::load(const std::filesystem::path& path,
                         ItemCatalogue& out,
                         std::string& error) {
    try {
        if (!std::filesystem::is_regular_file(path) || std::filesystem::file_size(path) > kMaxBytes)
            throw std::invalid_argument("missing, nonregular or oversized file");
        std::ifstream input(path, std::ios::binary);
        if (!input)
            throw std::invalid_argument("unreadable file");
        // Bound the captured read even if the file grows after file_size().
        std::string bytes(kMaxBytes + 1, '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        bytes.resize(static_cast<std::size_t>(input.gcount()));
        if (input.bad() || (!input.eof() && input.fail()))
            throw std::invalid_argument("read failure");
        return parse(bytes, out, error);
    } catch (const std::exception& exception) {
        error = std::string("Item catalogue: ") + exception.what();
        return false;
    }
}

const ItemDefinition* ItemCatalogue::FindByName(std::string_view name) const {
    const auto it = m_items.find(name);
    return it == m_items.end() ? nullptr : &it->second;
}
const ItemDefinition* ItemCatalogue::FindByType(std::uint32_t type) const {
    const auto it = m_types.find(type);
    return it == m_types.end() ? nullptr : FindByName(it->second);
}
bool ItemCatalogue::ValidateGameAdmission(std::string& error) const {
    const auto* seed = FindByName("farming-seed");
    if (!seed || seed->payload != 0) {
        error = "Item catalogue requires farming-seed with payload zero.";
        return false;
    }
    for (const auto& [name, item] : m_items) {
        if (item.render) {
            error = "Item catalogue has an unresolved render reference: " + name;
            return false;
        }
    }
    error.clear();
    return true;
}
} // namespace luminumbra::game
