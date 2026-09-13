#include "ViewportProtocol.h"
#include "authoring/PrefabDigest.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>
#include <string_view>

namespace Luminumbra::Viewport {
namespace {
constexpr std::uint64_t kMaxInteger = (1ull << 53) - 1;
void Require(bool value, const char* message) {
    if (!value)
        throw std::invalid_argument(message);
}
void Fields(const Json& object, std::initializer_list<const char*> fields) {
    Require(object.is_object() && object.size() == fields.size(), "Viewport object members");
    for (const char* field : fields)
        Require(object.contains(field), "Viewport object member missing");
}
bool Hex(const Json& value, std::size_t length) {
    if (!value.is_string())
        return false;
    const auto& text = value.get_ref<const std::string&>();
    return text.size() == length && text.find_first_not_of("0123456789abcdef") == std::string::npos;
}
std::uint64_t Integer(const Json& value, std::uint64_t low, std::uint64_t high) {
    Require(value.is_number_unsigned() ||
                (value.is_number_integer() && value.get<std::int64_t>() >= 0),
            "Viewport integer type");
    const auto result = value.get<std::uint64_t>();
    Require(result >= low && result <= high, "Viewport integer bounds");
    return result;
}
double Number(const Json& value) {
    Require(value.is_number(), "Viewport numeric type");
    const double result = value.get<double>();
    Require(std::isfinite(result), "Viewport nonfinite number");
    return result;
}
void Matrix(const Json& value) {
    Require(value.is_array() && value.size() == 16, "Viewport matrix shape");
    for (const auto& number : value)
        Require(std::abs(Number(number)) <= 1e12, "Viewport matrix bound");
}
bool Instance(const std::string& text) {
    const auto alpha = [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    };
    return !text.empty() && text.size() <= 128 && alpha(text[0]) &&
           std::all_of(text.begin(), text.end(), [&](char c) {
               return alpha(c) || (c >= '0' && c <= '9') ||
                      std::string_view("_.:-").find(c) != std::string_view::npos;
           });
}
void State(const Json& state) {
    Fields(state,
           {"generation_id",
            "manifest_sha256",
            "scene_revision",
            "camera_revision",
            "width",
            "height",
            "near_plane",
            "far_plane",
            "view",
            "projection",
            "locals"});
    Require(Hex(state.at("generation_id"), 32) && Hex(state.at("manifest_sha256"), 64),
            "Viewport generation pin");
    Integer(state.at("scene_revision"), 0, kMaxInteger);
    Integer(state.at("camera_revision"), 0, kMaxInteger);
    Integer(state.at("width"), 1, 1280);
    Integer(state.at("height"), 1, 720);
    const double near = Number(state.at("near_plane")), far = Number(state.at("far_plane"));
    Require(near >= .001 && near < far && far <= 1e6, "Viewport clip planes");
    Matrix(state.at("view"));
    Matrix(state.at("projection"));
    const auto& locals = state.at("locals");
    Require(locals.is_array() && locals.size() <= 1024, "Viewport local count");
    std::set<std::pair<std::string, std::string>> nodes;
    std::set<std::string> instances;
    for (const auto& local : locals) {
        Fields(local, {"instance_id", "node_id", "matrix"});
        Require(local.at("instance_id").is_string() && local.at("node_id").is_string(),
                "Viewport local identity type");
        const auto instance = local.at("instance_id").get<std::string>(),
                   node = local.at("node_id").get<std::string>();
        Require(Instance(instance) && !node.empty() && node.size() <= 128 &&
                    std::all_of(node.begin(),
                                node.end(),
                                [](unsigned char c) { return c >= 32 && c < 127; }),
                "Viewport local identity");
        Require(nodes.emplace(instance, node).second, "Viewport duplicate local");
        instances.insert(instance);
        Matrix(local.at("matrix"));
    }
    Require(instances.size() <= 64, "Viewport instance bound");
}
std::uint64_t Little(std::span<const std::uint8_t> bytes) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i)
        value |= static_cast<std::uint64_t>(bytes[i]) << (8 * i);
    return value;
}
void Append(std::vector<std::uint8_t>& bytes, std::uint64_t value, unsigned count) {
    for (unsigned i = 0; i < count; ++i)
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
}
Json Parse(std::span<const std::uint8_t> bytes) {
    std::vector<std::set<std::string>> keys;
    return Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& value) {
        Require(depth <= 32, "Viewport JSON nesting");
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        if (event == Json::parse_event_t::key)
            Require(keys.back().insert(value.get<std::string>()).second,
                    "Viewport duplicate JSON member");
        if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        return true;
    });
}
} // namespace
Key DecodeKey(const std::string& hex) {
    Require(Hex(Json(hex), 64), "Viewport key must be 32 bytes lowercase hex");
    Key key{};
    for (std::size_t i = 0; i < key.size(); ++i) {
        auto nibble = [](char c) {
            return c <= '9' ? c - '0' : c - 'a' + 10;
        };
        key[i] = static_cast<std::uint8_t>((nibble(hex[2 * i]) << 4) | nibble(hex[2 * i + 1]));
    }
    return key;
}
Key Mac(std::span<const std::uint8_t> message, const Key& key) {
    std::vector<std::uint8_t> inner(64, 0x36), outer(64, 0x5c);
    for (std::size_t i = 0; i < key.size(); ++i) {
        inner[i] ^= key[i];
        outer[i] ^= key[i];
    }
    inner.insert(inner.end(), message.begin(), message.end());
    const auto digest = DecodeKey(Authoring::Detail::Sha256(inner));
    outer.insert(outer.end(), digest.begin(), digest.end());
    return DecodeKey(Authoring::Detail::Sha256(outer));
}
std::size_t RecordSize(std::span<const std::uint8_t> prefix, bool input_state_only) {
    Require(prefix.size() == 16 && std::equal(prefix.begin(), prefix.begin() + 4, "LVP1"),
            "Viewport record magic");
    const auto header = Little(prefix.subspan(4, 4)), payload = Little(prefix.subspan(8, 8));
    Require(header > 0 && header <= kHeaderLimit && payload <= kPayloadLimit &&
                (!input_state_only || payload == 0),
            "Viewport record size");
    return static_cast<std::size_t>(48 + header + payload);
}
void Validate(const Record& record) {
    const auto& header = record.header;
    Require(header.is_object() && header.contains("kind") && header.at("kind").is_string(),
            "Viewport record kind");
    const auto kind = header.at("kind").get<std::string>();
    if (kind == "stop")
        Fields(header, {"kind", "session", "sequence"});
    else if (kind == "state")
        Fields(header, {"kind", "session", "sequence", "state"});
    else {
        Require(kind == "frame", "Viewport record kind");
        Fields(header, {"kind", "session", "sequence", "state", "planes_sha256"});
    }
    Require(Hex(header.at("session"), 32), "Viewport session");
    Integer(header.at("sequence"), 1, kMaxInteger);
    if (kind != "stop")
        State(header.at("state"));
    if (kind != "frame") {
        Require(record.payload.empty(), "Viewport unexpected input payload");
        return;
    }
    const auto pixels = header.at("state").at("width").get<std::size_t>() *
                        header.at("state").at("height").get<std::size_t>();
    Require(record.payload.size() == 9 * pixels && Hex(header.at("planes_sha256"), 64) &&
                header.at("planes_sha256") == Authoring::Detail::Sha256(record.payload),
            "Viewport frame planes");
    for (std::size_t i = 0; i < pixels; ++i) {
        const auto bits = static_cast<std::uint32_t>(
            Little(std::span(record.payload).subspan(4 * pixels + 4 * i, 4)));
        const auto depth = std::bit_cast<float>(bits),
                   covered = static_cast<float>(record.payload[8 * pixels + i]);
        Require(std::isfinite(depth) && depth >= 0 && depth <= 1 &&
                    covered == static_cast<float>(depth > 0) &&
                    record.payload[4 * i + 3] == 255 * covered,
                "Viewport frame depth/coverage/alpha");
    }
}
std::vector<std::uint8_t> Encode(const Record& record, const Key& key) {
    Validate(record);
    const auto text = record.header.dump();
    Require(text.size() <= kHeaderLimit && record.payload.size() <= kPayloadLimit,
            "Viewport encode bounds");
    std::vector<std::uint8_t> signed_bytes{'L', 'V', 'P', '1'};
    Append(signed_bytes, text.size(), 4);
    Append(signed_bytes, record.payload.size(), 8);
    signed_bytes.insert(signed_bytes.end(), text.begin(), text.end());
    signed_bytes.insert(signed_bytes.end(), record.payload.begin(), record.payload.end());
    const auto signature = Mac(signed_bytes, key);
    signed_bytes.insert(signed_bytes.begin() + 16, signature.begin(), signature.end());
    return signed_bytes;
}
Record Decode(std::span<const std::uint8_t> bytes, const Key& key) {
    Require(bytes.size() >= 48 && RecordSize(bytes.first(16), false) == bytes.size(),
            "Viewport incomplete record");
    std::vector<std::uint8_t> signed_bytes(bytes.begin(), bytes.begin() + 16);
    signed_bytes.insert(signed_bytes.end(), bytes.begin() + 48, bytes.end());
    const auto expected = Mac(signed_bytes, key);
    unsigned difference = 0;
    for (std::size_t i = 0; i < 32; ++i)
        difference |= bytes[16 + i] ^ expected[i];
    Require(difference == 0, "Viewport authentication");
    const auto count = static_cast<std::size_t>(Little(bytes.subspan(4, 4)));
    Record result{Parse(bytes.subspan(48, count)), {bytes.begin() + 48 + count, bytes.end()}};
    Validate(result);
    return result;
}
void StateGate::Accept(const Json& header) {
    Validate({header, {}});
    const auto kind = header.at("kind").get<std::string>();
    Require(kind == "state" || kind == "stop", "Viewport host input kind");
    if (!m_last.is_null()) {
        Require(header.at("session") == m_last.at("session"), "Viewport changed session");
        Require(header.at("sequence").get<std::uint64_t>() >
                    m_last.at("sequence").get<std::uint64_t>(),
                "Viewport replay");
        Require(m_last.at("kind") != "stop", "Viewport closed session");
    }
    if (kind == "state") {
        const auto& state = header.at("state");
        const auto generation = state.at("generation_id").get<std::string>(),
                   manifest = state.at("manifest_sha256").get<std::string>();
        const auto found = m_generations.find(generation);
        Require(found == m_generations.end() || found->second == manifest,
                "Viewport changed immutable manifest");
        Require(found != m_generations.end() || m_generations.size() < 64,
                "Viewport generation session bound");
        if (!m_last.is_null()) {
            const auto& previous = m_last.at("state");
            // Both revisions remain monotonic even when coalescing removes an
            // intermediate generation transition from the child input stream.
            Require(state.at("scene_revision") >= previous.at("scene_revision") &&
                        state.at("camera_revision") >= previous.at("camera_revision"),
                    "Viewport revision rollback");
            bool same_camera = true;
            for (const char* field :
                 {"view", "projection", "width", "height", "near_plane", "far_plane"})
                same_camera &= state.at(field) == previous.at(field);
            Require(same_camera || state.at("camera_revision") > previous.at("camera_revision"),
                    "Viewport changed camera without revision");
            if (generation == previous.at("generation_id").get<std::string>()) {
                Require(state.at("locals") == previous.at("locals") ||
                            state.at("scene_revision") > previous.at("scene_revision"),
                        "Viewport changed scene without revision");
            } else
                Require(state.at("scene_revision") > previous.at("scene_revision"),
                        "Viewport generation barrier");
        }
        m_generations[generation] = manifest;
    }
    m_last = header;
}
} // namespace Luminumbra::Viewport
