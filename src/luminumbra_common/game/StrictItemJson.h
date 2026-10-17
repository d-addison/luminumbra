#pragma once

#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace luminumbra::game::detail {
// JSON's default object parser discards duplicate keys. Keep that refusal explicit.
inline nlohmann::json ParseUniqueKeys(std::string_view bytes) {
    std::vector<std::set<std::string>> keys;
    bool duplicate = false;
    auto callback = [&](int, nlohmann::json::parse_event_t event, nlohmann::json& value) {
        using Event = nlohmann::json::parse_event_t;
        if (event == Event::object_start)
            keys.emplace_back();
        else if (event == Event::key)
            duplicate |= !keys.back().insert(value.get<std::string>()).second;
        else if (event == Event::object_end)
            keys.pop_back();
        return true;
    };
    auto document = nlohmann::json::parse(bytes.begin(), bytes.end(), callback);
    if (duplicate)
        throw std::invalid_argument("duplicate JSON key");
    return document;
}
} // namespace luminumbra::game::detail
