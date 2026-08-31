#pragma once

//.lmesh v2 (magic "LMS2") and sibling.lanim (magic "LANM") binary formats.
//
// v1.lmesh files (magic "LMSH") are untouched by this header: the v1 layout
// and writer remain byte-identical. LMS2 is a separate, additive format for
// skinned meshes produced by tools/asset_processor.cpp.
//
// Format constraints:
//   - joints/weights are u8x4 per vertex (weights normalized so they sum to
//     255 exactly; deterministic largest-remainder quantization),
//   - at most 256 joints per skeleton (joint indices fit in a u8),
//   -.lanim tracks are keyed by the FNV-1a 32-bit hash of the joint name.

#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace luminumbra::animation {

inline constexpr uint32_t kLms2Magic =
    static_cast<uint32_t>('L') | (static_cast<uint32_t>('M') << 8) |
    (static_cast<uint32_t>('S') << 16) | (static_cast<uint32_t>('2') << 24);

inline constexpr uint32_t kLanimMagic =
    static_cast<uint32_t>('L') | (static_cast<uint32_t>('A') << 8) |
    (static_cast<uint32_t>('N') << 16) | (static_cast<uint32_t>('M') << 24);

inline constexpr uint32_t kLms2Version = 1;
inline constexpr uint32_t kLanimVersion = 1;
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

struct LanimTrackHeader {
    uint32_t jointNameHash = 0;
    uint32_t targetType = 0; // AnimTargetType
    uint32_t keyCount = 0;
    uint32_t componentCount = 0; // 3 for T/S, 4 for R
};

#pragma pack(pop)

static_assert(sizeof(Lms2Header) == 36, "LMS2 header is 36 bytes");
static_assert(sizeof(SkinnedVertexData) == 40, "LMS2 vertex is 40 bytes");
static_assert(sizeof(Lms2Joint) == 112, "LMS2 joint record is 112 bytes");
static_assert(sizeof(LanimHeader) == 16, "LANM header is 16 bytes");
static_assert(sizeof(LanimTrackHeader) == 16, "LANM track header is 16 bytes");

struct SkinnedMeshAsset {
    Lms2Header header;
    std::vector<SkinnedVertexData> vertices;
    std::vector<uint32_t> indices;
    std::vector<Lms2Joint> joints;
};

struct AnimTrack {
    LanimTrackHeader header;
    std::vector<float> times;  // keyCount entries, ascending
    std::vector<float> values; // keyCount * componentCount entries
};

struct AnimClipAsset {
    LanimHeader header;
    std::vector<AnimTrack> tracks;
};

inline bool LoadSkinnedMeshAsset(const std::string& path, SkinnedMeshAsset& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;

    in.read(reinterpret_cast<char*>(&out.header), sizeof(out.header));
    if (!in || out.header.magic != kLms2Magic || out.header.version != kLms2Version)
        return false;
    if (out.header.jointCount > kMaxJointsPerSkeleton)
        return false;

    out.vertices.resize(out.header.vertexCount);
    out.indices.resize(out.header.indexCount);
    out.joints.resize(out.header.jointCount);
    in.read(reinterpret_cast<char*>(out.vertices.data()),
            static_cast<std::streamsize>(out.vertices.size() * sizeof(SkinnedVertexData)));
    in.read(reinterpret_cast<char*>(out.indices.data()),
            static_cast<std::streamsize>(out.indices.size() * sizeof(uint32_t)));
    in.read(reinterpret_cast<char*>(out.joints.data()),
            static_cast<std::streamsize>(out.joints.size() * sizeof(Lms2Joint)));
    return static_cast<bool>(in);
}

inline bool LoadAnimClipAsset(const std::string& path, AnimClipAsset& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;

    in.read(reinterpret_cast<char*>(&out.header), sizeof(out.header));
    if (!in || out.header.magic != kLanimMagic || out.header.version != kLanimVersion)
        return false;

    out.tracks.resize(out.header.trackCount);
    for (AnimTrack& track : out.tracks) {
        in.read(reinterpret_cast<char*>(&track.header), sizeof(track.header));
        if (!in)
            return false;
        track.times.resize(track.header.keyCount);
        track.values.resize(static_cast<size_t>(track.header.keyCount) *
                            track.header.componentCount);
        in.read(reinterpret_cast<char*>(track.times.data()),
                static_cast<std::streamsize>(track.times.size() * sizeof(float)));
        in.read(reinterpret_cast<char*>(track.values.data()),
                static_cast<std::streamsize>(track.values.size() * sizeof(float)));
        if (!in)
            return false;
    }
    return true;
}

} // namespace luminumbra::animation
