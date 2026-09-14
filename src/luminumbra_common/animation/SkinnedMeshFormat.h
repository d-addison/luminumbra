#pragma once

//.lmesh v2 (magic "LMS2") and sibling.lanim (magic "LANM") binary formats.
//
// v1.lmesh files (magic "LMSH") are untouched by this header: the v1 layout
// is unchanged. LMS2 is a separate, additive format for
// skinned meshes produced by tools/asset_processor.cpp.
//
// Format constraints:
//   - joints/weights are u8x4 per vertex (weights normalized so they sum to
//     255 exactly; deterministic largest-remainder quantization),
//   - at most 256 joints per skeleton (joint indices fit in a u8),
//   -.lanim tracks are keyed by the FNV-1a 32-bit hash of the joint name.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace luminumbra::animation {

inline constexpr uint32_t kLms2Magic =
    static_cast<uint32_t>('L') | (static_cast<uint32_t>('M') << 8) |
    (static_cast<uint32_t>('S') << 16) | (static_cast<uint32_t>('2') << 24);

inline constexpr uint32_t kLanimMagic =
    static_cast<uint32_t>('L') | (static_cast<uint32_t>('A') << 8) |
    (static_cast<uint32_t>('N') << 16) | (static_cast<uint32_t>('M') << 24);

inline constexpr uint32_t kLms2Version = 1;
inline constexpr uint32_t kLanimLegacyVersion = 1;
inline constexpr uint32_t kLanimVersion = 2;
inline constexpr uint32_t kMaxJointsPerSkeleton = 256;

// FNV-1a 32-bit hash used to key.lanim tracks by joint name.
inline constexpr uint32_t HashJointName(std::string_view name) {
    uint32_t hash = 2166136261u;
    for (const char c : name) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 16777619u;
    }
    return hash;
}

#pragma pack(push, 1)

struct Lms2Header {
    uint32_t magic = kLms2Magic;
    uint32_t version = kLms2Version;
    uint32_t vertexCount = 0;
    uint32_t indexCount = 0;
    uint32_t jointCount = 0;
    float boundingSphere[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

// 40-byte skinned vertex: v1 layout (pos/norm/uv) + u8x4 joints + u8x4 weights.
struct SkinnedVertexData {
    float pos[3];
    float norm[3];
    float uv[2];
    uint8_t joints[4];
    uint8_t weights[4];
};

// 112-byte joint record: name hash, parent index (-1 = root), inverse bind
// matrix (column-major, 16 floats) and the local bind TRS pose.
struct Lms2Joint {
    uint32_t nameHash = 0;
    int32_t parentIndex = -1;
    float inverseBind[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float localTranslation[3] = {0.0f, 0.0f, 0.0f};
    float localRotation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
    float localScale[3] = {1.0f, 1.0f, 1.0f};
};

struct LanimHeader {
    uint32_t magic = kLanimMagic;
    uint32_t version = kLanimVersion;
    uint32_t trackCount = 0;
    float duration = 0.0f;
};

enum class AnimTargetType : uint32_t {
    Translation = 0,
    Rotation = 1,
    Scale = 2,
};

enum class AnimInterpolation : uint32_t {
    LegacyLinear = 0, // LANM v1: vector lerp and quaternion nlerp.
    Step = 1,
    Linear = 2,      // glTF: vector lerp and shortest-arc quaternion slerp.
    CubicSpline = 3, // glTF Hermite triples: in tangent, value, out tangent.
};

struct LanimTrackHeader {
    uint32_t jointNameHash = 0;
    uint32_t targetType = 0; // AnimTargetType
    uint32_t keyCount = 0;
    uint32_t componentCount = 0; // 3 for T/S, 4 for R
};

struct LanimTrackHeaderV2 {
    LanimTrackHeader track;
    uint32_t interpolation = static_cast<uint32_t>(AnimInterpolation::Linear);
};

#pragma pack(pop)

static_assert(sizeof(Lms2Header) == 36, "LMS2 header is 36 bytes");
static_assert(sizeof(SkinnedVertexData) == 40, "LMS2 vertex is 40 bytes");
static_assert(sizeof(Lms2Joint) == 112, "LMS2 joint record is 112 bytes");
static_assert(sizeof(LanimHeader) == 16, "LANM header is 16 bytes");
static_assert(sizeof(LanimTrackHeader) == 16, "LANM track header is 16 bytes");
static_assert(sizeof(LanimTrackHeaderV2) == 20, "LANM v2 track header is 20 bytes");

struct SkinnedMeshAsset {
    Lms2Header header;
    std::vector<SkinnedVertexData> vertices;
    std::vector<uint32_t> indices;
    std::vector<Lms2Joint> joints;
};

struct AnimTrack {
    LanimTrackHeader header;
    AnimInterpolation interpolation = AnimInterpolation::LegacyLinear;
    std::vector<float> times;  // keyCount entries, ascending
    std::vector<float> values; // keyCount * componentCount, times three for cubic
};

struct AnimClipAsset {
    LanimHeader header;
    std::vector<AnimTrack> tracks;
};

inline bool ValidateAnimTrack(const AnimTrack& track, float duration) {
    const auto& h = track.header;
    if (h.targetType > static_cast<uint32_t>(AnimTargetType::Scale) || h.keyCount == 0 ||
        h.componentCount !=
            (h.targetType == static_cast<uint32_t>(AnimTargetType::Rotation) ? 4u : 3u) ||
        static_cast<uint32_t>(track.interpolation) >
            static_cast<uint32_t>(AnimInterpolation::CubicSpline) ||
        !std::isfinite(duration) || duration < 0)
        return false;
    const bool cubic = track.interpolation == AnimInterpolation::CubicSpline;
    const uint64_t stride = h.componentCount * (cubic ? 3u : 1u);
    if ((cubic && h.keyCount < 2) || track.times.size() != h.keyCount ||
        track.values.size() != uint64_t(h.keyCount) * stride)
        return false;
    for (size_t i = 0; i < track.times.size(); ++i) {
        const float time = track.times[i];
        if (!std::isfinite(time) || time < 0 || time > duration ||
            (i && time <= track.times[i - 1]))
            return false;
        if (h.targetType == static_cast<uint32_t>(AnimTargetType::Rotation)) {
            double norm = 0;
            const size_t at = i * static_cast<size_t>(stride) + (cubic ? h.componentCount : 0);
            for (size_t c = 0; c < 4; ++c)
                norm += double(track.values[at + c]) * track.values[at + c];
            if (!std::isfinite(norm) || norm == 0 ||
                (track.interpolation != AnimInterpolation::LegacyLinear &&
                 std::abs(norm - 1.0) > 1e-3))
                return false;
        }
    }
    return std::all_of(
        track.values.begin(), track.values.end(), [](float value) { return std::isfinite(value); });
}

inline bool LoadSkinnedMeshAsset(const std::string& path, SkinnedMeshAsset& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    const auto size = in.tellg();
    if (size < static_cast<std::streamoff>(sizeof(Lms2Header)))
        return false;
    in.seekg(0);
    SkinnedMeshAsset candidate;
    auto& h = candidate.header;
    in.read(reinterpret_cast<char*>(&h), sizeof(h));
    if (!in || h.magic != kLms2Magic || h.version != kLms2Version || h.jointCount == 0 ||
        h.jointCount > kMaxJointsPerSkeleton || h.vertexCount == 0 || h.indexCount == 0 ||
        h.indexCount % 3 != 0 || h.boundingSphere[3] < 0)
        return false;
    for (const float value : h.boundingSphere)
        if (!std::isfinite(value))
            return false;
    const uint64_t expected =
        sizeof(Lms2Header) + uint64_t(h.vertexCount) * sizeof(SkinnedVertexData) +
        uint64_t(h.indexCount) * sizeof(uint32_t) + uint64_t(h.jointCount) * sizeof(Lms2Joint);
    if (expected != static_cast<uint64_t>(size))
        return false;
    candidate.vertices.resize(h.vertexCount);
    candidate.indices.resize(h.indexCount);
    candidate.joints.resize(h.jointCount);
    in.read(reinterpret_cast<char*>(candidate.vertices.data()),
            static_cast<std::streamsize>(candidate.vertices.size() * sizeof(SkinnedVertexData)));
    in.read(reinterpret_cast<char*>(candidate.indices.data()),
            static_cast<std::streamsize>(candidate.indices.size() * sizeof(uint32_t)));
    in.read(reinterpret_cast<char*>(candidate.joints.data()),
            static_cast<std::streamsize>(candidate.joints.size() * sizeof(Lms2Joint)));
    if (!in)
        return false;
    for (const auto& vertex : candidate.vertices) {
        unsigned sum = 0;
        for (size_t lane = 0; lane < 4; ++lane) {
            if (vertex.joints[lane] >= h.jointCount)
                return false;
            sum += vertex.weights[lane];
        }
        if (sum != 255)
            return false;
        for (const float value : vertex.pos)
            if (!std::isfinite(value))
                return false;
        for (const float value : vertex.norm)
            if (!std::isfinite(value))
                return false;
        for (const float value : vertex.uv)
            if (!std::isfinite(value))
                return false;
    }
    for (const uint32_t index : candidate.indices)
        if (index >= h.vertexCount)
            return false;
    std::unordered_set<uint32_t> identities;
    for (size_t j = 0; j < candidate.joints.size(); ++j) {
        const auto& joint = candidate.joints[j];
        if (joint.parentIndex < -1 ||
            (joint.parentIndex >= 0 && static_cast<size_t>(joint.parentIndex) >= j) ||
            !identities.insert(joint.nameHash).second)
            return false;
        for (const float value : joint.inverseBind)
            if (!std::isfinite(value))
                return false;
        if (joint.inverseBind[3] != 0 || joint.inverseBind[7] != 0 || joint.inverseBind[11] != 0 ||
            joint.inverseBind[15] != 1)
            return false;
        for (const float value : joint.localTranslation)
            if (!std::isfinite(value))
                return false;
        for (const float value : joint.localScale)
            if (!std::isfinite(value))
                return false;
        double norm = 0;
        for (const float value : joint.localRotation)
            norm += double(value) * value;
        if (!std::isfinite(norm) || norm == 0)
            return false;
    }
    out = std::move(candidate);
    return true;
}

inline bool LoadAnimClipAsset(const std::string& path, AnimClipAsset& out) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        return false;
    const auto size = in.tellg();
    if (size < static_cast<std::streamoff>(sizeof(LanimHeader)))
        return false;
    uint64_t remaining = static_cast<uint64_t>(size) - sizeof(LanimHeader);
    in.seekg(0);
    AnimClipAsset candidate;
    in.read(reinterpret_cast<char*>(&candidate.header), sizeof(candidate.header));
    if (!in || candidate.header.magic != kLanimMagic ||
        (candidate.header.version != kLanimVersion &&
         candidate.header.version != kLanimLegacyVersion) ||
        !std::isfinite(candidate.header.duration) || candidate.header.duration < 0)
        return false;
    const bool modern = candidate.header.version == kLanimVersion;
    const size_t headerSize = modern ? sizeof(LanimTrackHeaderV2) : sizeof(LanimTrackHeader);
    if (candidate.header.trackCount > remaining / headerSize)
        return false;
    candidate.tracks.reserve(candidate.header.trackCount);
    std::unordered_set<uint64_t> targets;
    for (uint32_t t = 0; t < candidate.header.trackCount; ++t) {
        if (remaining < headerSize)
            return false;
        remaining -= headerSize;
        AnimTrack track;
        in.read(reinterpret_cast<char*>(&track.header), sizeof(track.header));
        if (modern) {
            uint32_t mode = 0;
            in.read(reinterpret_cast<char*>(&mode), sizeof(mode));
            if (mode < static_cast<uint32_t>(AnimInterpolation::Step) ||
                mode > static_cast<uint32_t>(AnimInterpolation::CubicSpline))
                return false;
            track.interpolation = static_cast<AnimInterpolation>(mode);
        }
        if (!in || track.header.targetType > static_cast<uint32_t>(AnimTargetType::Scale) ||
            track.header.componentCount != (track.header.targetType == 1 ? 4u : 3u))
            return false;
        const uint64_t values = uint64_t(track.header.keyCount) * track.header.componentCount *
                                (track.interpolation == AnimInterpolation::CubicSpline ? 3u : 1u);
        const uint64_t bytes = (uint64_t(track.header.keyCount) + values) * sizeof(float);
        if (bytes > remaining ||
            !targets.insert((uint64_t(track.header.jointNameHash) << 32) | track.header.targetType)
                 .second)
            return false;
        remaining -= bytes;
        track.times.resize(track.header.keyCount);
        track.values.resize(static_cast<size_t>(values));
        in.read(reinterpret_cast<char*>(track.times.data()),
                static_cast<std::streamsize>(track.times.size() * sizeof(float)));
        in.read(reinterpret_cast<char*>(track.values.data()),
                static_cast<std::streamsize>(track.values.size() * sizeof(float)));
        if (!in || !ValidateAnimTrack(track, candidate.header.duration))
            return false;
        candidate.tracks.push_back(std::move(track));
    }
    if (remaining != 0)
        return false;
    out = std::move(candidate);
    return true;
}

} // namespace luminumbra::animation
