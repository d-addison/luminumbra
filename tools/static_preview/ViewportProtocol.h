#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <span>
#include <vector>

namespace Luminumbra::Viewport {
using Json = nlohmann::json;
using Key = std::array<std::uint8_t, 32>;
inline constexpr std::size_t kHeaderLimit = 1024 * 1024;
inline constexpr std::size_t kPayloadLimit = 1280 * 720 * 9;
struct Record {
    Json header;
    std::vector<std::uint8_t> payload;
};
Key DecodeKey(const std::string& hex);
Key Mac(std::span<const std::uint8_t> message, const Key& key);
std::size_t RecordSize(std::span<const std::uint8_t> prefix, bool input_state_only);
std::vector<std::uint8_t> Encode(const Record& record, const Key& key);
Record Decode(std::span<const std::uint8_t> bytes, const Key& key);
void Validate(const Record& record);
class StateGate {
public:
    void Accept(const Json& header);
    const Json& last() const {
        return m_last;
    }

private:
    Json m_last;
    std::map<std::string, std::string> m_generations;
};
} // namespace Luminumbra::Viewport
