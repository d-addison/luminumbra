#include "ModManifest.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <utility>

namespace luminumbra::mods {
namespace {

struct Version {
    std::array<std::uint64_t, 3> core{};
    std::string_view prerelease;
};

bool IsDigit(char byte) {
    return byte >= '0' && byte <= '9';
}

bool IsNumeric(std::string_view text) {
    return !text.empty() && std::all_of(text.begin(), text.end(), IsDigit);
}

bool Identifiers(std::string_view text, bool prerelease) {
    while (!text.empty()) {
        const auto dot = text.find('.');
        const auto part = text.substr(0, dot);
        if (part.empty() || !std::all_of(part.begin(), part.end(), [](char byte) {
                return IsDigit(byte) || (byte >= 'a' && byte <= 'z') ||
                       (byte >= 'A' && byte <= 'Z') || byte == '-';
            })) {
            return false;
        }
        if (prerelease && IsNumeric(part) && part.size() > 1 && part.front() == '0') {
            return false;
        }
        if (dot == std::string_view::npos) {
            return true;
        }
        text.remove_prefix(dot + 1);
    }
    return false;
}

bool ParseVersion(std::string_view text, Version& out) {
    if (text.empty() || text.size() > 128) {
        return false;
    }
    const auto plus = text.find('+');
    if (plus != std::string_view::npos) {
        if (!Identifiers(text.substr(plus + 1), false)) {
            return false;
        }
        text = text.substr(0, plus);
    }
    const auto dash = text.find('-');
    out.prerelease = {};
    if (dash != std::string_view::npos) {
        out.prerelease = text.substr(dash + 1);
        if (!Identifiers(out.prerelease, true)) {
            return false;
        }
        text = text.substr(0, dash);
    }
    for (std::size_t index = 0; index < out.core.size(); ++index) {
        const auto dot = text.find('.');
        const auto part = text.substr(0, dot);
        if (!IsNumeric(part) || (part.size() > 1 && part.front() == '0') ||
            (index < 2) != (dot != std::string_view::npos)) {
            return false;
        }
        const auto conversion =
            std::from_chars(part.data(), part.data() + part.size(), out.core[index]);
        if (conversion.ec != std::errc{}) {
            return false;
        }
        if (dot != std::string_view::npos) {
            text.remove_prefix(dot + 1);
        }
    }
    return true;
}

int Compare(const Version& left, const Version& right) {
    if (left.core != right.core) {
        return left.core < right.core ? -1 : 1;
    }
    auto lhs = left.prerelease;
    auto rhs = right.prerelease;
    if (lhs.empty() || rhs.empty()) {
        return lhs == rhs ? 0 : (lhs.empty() ? 1 : -1);
    }
    while (!lhs.empty() && !rhs.empty()) {
        const auto lhs_dot = lhs.find('.');
        const auto rhs_dot = rhs.find('.');
        const auto lhs_part = lhs.substr(0, lhs_dot);
        const auto rhs_part = rhs.substr(0, rhs_dot);
        if (lhs_part != rhs_part) {
            const bool lhs_numeric = IsNumeric(lhs_part);
            const bool rhs_numeric = IsNumeric(rhs_part);
            if (lhs_numeric != rhs_numeric) {
                return lhs_numeric ? -1 : 1;
            }
            if (lhs_numeric && lhs_part.size() != rhs_part.size()) {
                return lhs_part.size() < rhs_part.size() ? -1 : 1;
            }
            return lhs_part < rhs_part ? -1 : 1;
        }
        lhs = lhs_dot == std::string_view::npos ? std::string_view{} : lhs.substr(lhs_dot + 1);
        rhs = rhs_dot == std::string_view::npos ? std::string_view{} : rhs.substr(rhs_dot + 1);
    }
    return lhs == rhs ? 0 : (lhs.empty() ? -1 : 1);
}

bool Space(char byte) {
    return byte == ' ' || byte == '\t' || byte == '\n' || byte == '\r';
}

std::string_view Trim(std::string_view text) {
    while (!text.empty() && Space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && Space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

bool Comparator(std::string_view token,
                const Version& candidate,
                bool& admits_prerelease,
                bool& matches) {
    if (token == "*") {
        matches = true;
        return true;
    }
    std::string_view operation;
    if (!token.empty() && std::string_view("<>=^~").find(token.front()) != std::string_view::npos) {
        const auto count = token.size() > 1 && token[1] == '=' ? 2U : 1U;
        operation = token.substr(0, count);
        token.remove_prefix(count);
    }
    Version boundary;
    if (!ParseVersion(token, boundary)) {
        return false;
    }
    admits_prerelease =
        admits_prerelease || (!boundary.prerelease.empty() && candidate.core == boundary.core);
    const int comparison = Compare(candidate, boundary);
    if (operation.empty() || operation == "=") {
        matches = comparison == 0;
    } else if (operation == "<" || operation == "<=") {
        matches = comparison < 0 || (operation == "<=" && comparison == 0);
    } else if (operation == ">" || operation == ">=") {
        matches = comparison > 0 || (operation == ">=" && comparison == 0);
    } else if (operation == "^" || operation == "~") {
        // Compare the unchanged prefix instead of incrementing an upper bound,
        // so UINT64_MAX components cannot overflow.
        std::size_t prefix = 2;
        if (operation == "^") {
            prefix = boundary.core[0] != 0 ? 1 : (boundary.core[1] != 0 ? 2 : 3);
        }
        matches = comparison >= 0 && std::equal(boundary.core.begin(),
                                                boundary.core.begin() + prefix,
                                                candidate.core.begin());
    } else {
        return false;
    }
    return true;
}

bool ParseRange(std::string_view text, const Version& candidate, bool& matches) {
    if (text.size() > 512) {
        return false;
    }
    matches = false;
    while (true) {
        const auto separator = text.find("||");
        auto clause = Trim(text.substr(0, separator));
        if (clause.empty()) {
            return false;
        }
        bool conjunction = true;
        bool admits_prerelease = candidate.prerelease.empty();
        while (!clause.empty()) {
            const auto end = clause.find_first_of(" \t\n\r");
            bool comparison = false;
            if (!Comparator(clause.substr(0, end), candidate, admits_prerelease, comparison)) {
                return false;
            }
            conjunction = conjunction && comparison;
            clause = end == std::string_view::npos ? std::string_view{} : Trim(clause.substr(end));
        }
        matches = matches || (conjunction && admits_prerelease);
        if (separator == std::string_view::npos) {
            return true;
        }
        text.remove_prefix(separator + 2);
    }
}

bool ValidId(std::string_view id) {
    if (id.empty() || id.size() > 128 || id.find('.') == std::string_view::npos) {
        return false;
    }
    bool segment = false;
    for (const char byte : id) {
        if (byte == '.') {
            if (!segment) {
                return false;
            }
            segment = false;
        } else {
            if (!(IsDigit(byte) || (byte >= 'a' && byte <= 'z') || byte == '_')) {
                return false;
            }
            segment = true;
        }
    }
    return segment;
}

bool ValidateRelations(const std::vector<ModDependency>& relations, std::string& error) {
    if (relations.size() > kMaxModRelations) {
        error = "too many mod relations";
        return false;
    }
    std::set<std::string_view> ids;
    for (const auto& relation : relations) {
        bool ignored = false;
        if (!ValidId(relation.id) || !ids.insert(relation.id).second ||
            !ParseRange(relation.version, Version{}, ignored)) {
            error = "invalid or duplicate mod relation: " + relation.id.substr(0, 128);
            return false;
        }
    }
    return true;
}

bool ValidateHints(const std::vector<std::string>& hints, std::string& error) {
    if (hints.size() > kMaxModRelations) {
        error = "too many load order hints";
        return false;
    }
    std::set<std::string_view> ids;
    for (const auto& id : hints) {
        if (!ValidId(id) || !ids.insert(id).second) {
            error = "invalid or duplicate load order id: " + id.substr(0, 128);
            return false;
        }
    }
    return true;
}

std::vector<ModDependency>
ReadRelations(const nlohmann::json& document, const char* key, bool dependency) {
    std::vector<ModDependency> result;
    const auto found = document.find(key);
    if (found == document.end()) {
        return result;
    }
    if (!found->is_array() || found->size() > kMaxModRelations) {
        throw std::invalid_argument(std::string(key) +
                                    " must be an array of at most 128 relations");
    }
    for (const auto& entry : *found) {
        result.push_back({entry.at("id").get<std::string>(),
                          entry.at("version").get<std::string>(),
                          dependency ? entry.value("optional", false) : false});
    }
    return result;
}

std::vector<std::string> ReadHints(const nlohmann::json& order, const char* key) {
    const auto found = order.find(key);
    if (found == order.end()) {
        return {};
    }
    if (!found->is_array() || found->size() > kMaxModRelations) {
        throw std::invalid_argument(std::string(key) + " must be an array of at most 128 ids");
    }
    return found->get<std::vector<std::string>>();
}

} // namespace

core::ResidencyClass ModManifest::residency() const noexcept {
    return tier == ModTier::ClientCosmetic || tier == ModTier::ClientNative
               ? core::ResidencyClass::Render
               : core::ResidencyClass::Sim;
}

bool ModManifest::Validate(std::string& error) const {
    error.clear();
    Version parsed;
    if (!ValidId(id) || !ParseVersion(version, parsed)) {
        error = "invalid mod id or version: " + id.substr(0, 128);
        return false;
    }
    switch (tier) {
        case ModTier::ClientCosmetic:
        case ModTier::SimData:
        case ModTier::SimScript:
        case ModTier::SimNative:
        case ModTier::ClientNative:
            break;
        default:
            error = "invalid mod tier: " + id;
            return false;
    }
    return ValidateRelations(dependencies, error) && ValidateRelations(incompatible_with, error) &&
           ValidateHints(load_after, error) && ValidateHints(load_before, error);
}

bool ModManifest::Parse(std::string_view json, ModManifest& out, std::string& error) {
    error.clear();
    if (json.size() > kMaxManifestBytes) {
        error = "mod manifest exceeds 65536 bytes";
        return false;
    }
    try {
        // Bound nesting before recursive JSON parsing; refuse duplicate keys rather
        // than depending on a JSON library's first/last-key policy.
        std::vector<std::set<std::string>> object_keys;
        auto callback =
            [&object_keys](int depth, nlohmann::json::parse_event_t event, nlohmann::json& parsed) {
                if (depth > 16) {
                    throw std::invalid_argument("mod manifest nesting exceeds 16");
                }
                if (event == nlohmann::json::parse_event_t::object_start) {
                    object_keys.emplace_back();
                } else if (event == nlohmann::json::parse_event_t::object_end) {
                    object_keys.pop_back();
                } else if (event == nlohmann::json::parse_event_t::key &&
                           !object_keys.back().insert(parsed.get<std::string>()).second) {
                    throw std::invalid_argument("duplicate mod manifest key");
                }
                return true;
            };
        const auto document = nlohmann::json::parse(json.begin(), json.end(), callback);
        if (document.at("schema") != "luminumbra.mod.manifest.v1") {
            throw std::invalid_argument("unsupported mod manifest schema");
        }
        ModManifest result;
        result.id = document.at("id").get<std::string>();
        result.version = document.at("version").get<std::string>();
        const auto tier_name = document.at("tier").get<std::string>();
        if (tier_name == "client_cosmetic") {
            result.tier = ModTier::ClientCosmetic;
        } else if (tier_name == "sim_data") {
            result.tier = ModTier::SimData;
        } else if (tier_name == "sim_script") {
            result.tier = ModTier::SimScript;
        } else if (tier_name == "sim_native") {
            result.tier = ModTier::SimNative;
        } else if (tier_name == "client_native") {
            result.tier = ModTier::ClientNative;
        } else {
            throw std::invalid_argument("invalid mod tier: " + tier_name);
        }
        result.dependencies = ReadRelations(document, "dependencies", true);
        result.incompatible_with = ReadRelations(document, "incompatible_with", false);
        const auto order = document.find("load_order");
        if (order != document.end()) {
            if (!order->is_object()) {
                throw std::invalid_argument("load_order must be an object");
            }
            result.load_after = ReadHints(*order, "after");
            result.load_before = ReadHints(*order, "before");
        }
        if (!result.Validate(error)) {
            return false;
        }
        out = std::move(result);
        return true;
    } catch (const nlohmann::json::exception& exception) {
        error = std::string("invalid mod manifest JSON: ") + exception.what();
    } catch (const std::invalid_argument& exception) {
        error = exception.what();
    }
    return false;
}

bool ModVersionMatches(std::string_view version, std::string_view range) {
    Version parsed;
    bool matches = false;
    return ParseVersion(version, parsed) && ParseRange(range, parsed, matches) && matches;
}

} // namespace luminumbra::mods
