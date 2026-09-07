#include <algorithm>
#include <array>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#define CGLTF_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
#include "cgltf.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
#include "meshoptimizer.h" // This will now be found via the include path in CMake

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"

#include "luminumbra_common/animation/SkinnedMeshFormat.h"

struct Vertex {
    float pos[3];
    float norm[3];
    float uv[2];
};

struct LMeshHeader {
    uint32_t magic;
    uint32_t vertexCount;
    uint32_t indexCount;
    float boundingSphere[4];
};

namespace {

using luminumbra::animation::AnimTargetType;
using luminumbra::animation::HashJointName;
using luminumbra::animation::kMaxJointsPerSkeleton;
using luminumbra::animation::LanimHeader;
using luminumbra::animation::LanimTrackHeader;
using luminumbra::animation::Lms2Header;
using luminumbra::animation::Lms2Joint;
using luminumbra::animation::SkinnedVertexData;

// Deterministic largest-remainder quantization of float weights to u8 so the
// four quantized weights sum to exactly 255. Ties broken by lower lane index.
void QuantizeWeights(const float weights[4], uint8_t out[4]) {
    float total = weights[0] + weights[1] + weights[2] + weights[3];
    if (total <= 0.0f) {
        out[0] = 255;
        out[1] = 0;
        out[2] = 0;
        out[3] = 0;
        return;
    }

    int quantized[4];
    float remainders[4];
    int sum = 0;
    for (int i = 0; i < 4; ++i) {
        const float scaled = (weights[i] / total) * 255.0f;
        quantized[i] = static_cast<int>(scaled);
        remainders[i] = scaled - static_cast<float>(quantized[i]);
        sum += quantized[i];
    }

    int deficit = 255 - sum;
    while (deficit > 0) {
        int best = 0;
        for (int i = 1; i < 4; ++i) {
            if (remainders[i] > remainders[best])
                best = i;
        }
        quantized[best] += 1;
        remainders[best] = -1.0f;
        --deficit;
    }

    for (int i = 0; i < 4; ++i) {
        out[i] = static_cast<uint8_t>(std::clamp(quantized[i], 0, 255));
    }
}

std::string JointName(const cgltf_node* node, size_t fallbackIndex) {
    if (node->name && node->name[0] != '\0')
        return node->name;
    return "joint" + std::to_string(fallbackIndex);
}

std::string OutputStem(const std::string& output_path) {
    const size_t dot = output_path.find_last_of('.');
    const size_t sep = output_path.find_last_of("/\\");
    if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) {
        return output_path.substr(0, dot);
    }
    return output_path;
}

bool WriteAnimationClips(const cgltf_data* data, const std::string& output_path) {
    const std::string stem = OutputStem(output_path);

    for (size_t anim_idx = 0; anim_idx < data->animations_count; ++anim_idx) {
        const cgltf_animation* anim = &data->animations[anim_idx];

        LanimHeader header;
        std::vector<LanimTrackHeader> trackHeaders;
        std::vector<std::vector<float>> trackTimes;
        std::vector<std::vector<float>> trackValues;

        for (size_t ch = 0; ch < anim->channels_count; ++ch) {
            const cgltf_animation_channel* channel = &anim->channels[ch];
            if (!channel->target_node || !channel->sampler)
                continue;

            AnimTargetType targetType;
            uint32_t componentCount;
            switch (channel->target_path) {
                case cgltf_animation_path_type_translation:
                    targetType = AnimTargetType::Translation;
                    componentCount = 3;
                    break;
                case cgltf_animation_path_type_rotation:
                    targetType = AnimTargetType::Rotation;
                    componentCount = 4;
                    break;
                case cgltf_animation_path_type_scale:
                    targetType = AnimTargetType::Scale;
                    componentCount = 3;
                    break;
                default:
                    continue; // morph weights unsupported in v1.lanim
            }

            const cgltf_accessor* input = channel->sampler->input;
            const cgltf_accessor* output = channel->sampler->output;
            if (!input || !output || input->count == 0 || output->count < input->count)
                continue;

            const size_t nodeIndex = static_cast<size_t>(channel->target_node - data->nodes);
            const uint32_t nameHash = HashJointName(JointName(channel->target_node, nodeIndex));

            std::vector<float> times(input->count);
            std::vector<float> values(static_cast<size_t>(input->count) * componentCount);
            for (size_t k = 0; k < input->count; ++k) {
                cgltf_accessor_read_float(input, k, &times[k], 1);
                cgltf_accessor_read_float(output, k, &values[k * componentCount], componentCount);
                header.duration = std::max(header.duration, times[k]);
            }

            LanimTrackHeader trackHeader;
            trackHeader.jointNameHash = nameHash;
            trackHeader.targetType = static_cast<uint32_t>(targetType);
            trackHeader.keyCount = static_cast<uint32_t>(input->count);
            trackHeader.componentCount = componentCount;
            trackHeaders.push_back(trackHeader);
            trackTimes.push_back(std::move(times));
            trackValues.push_back(std::move(values));
        }

        if (trackHeaders.empty())
            continue;
        header.trackCount = static_cast<uint32_t>(trackHeaders.size());

        const std::string animName = (anim->name && anim->name[0] != '\0')
                                         ? std::string(anim->name)
                                         : ("anim" + std::to_string(anim_idx));
        const std::string animPath = stem + "." + animName + ".lanim";

        std::ofstream outFile(animPath, std::ios::binary);
        if (!outFile) {
            std::cerr << "Error: Could not open animation output file " << animPath << std::endl;
            return false;
        }
        outFile.write(reinterpret_cast<const char*>(&header), sizeof(header));
        for (size_t t = 0; t < trackHeaders.size(); ++t) {
            outFile.write(reinterpret_cast<const char*>(&trackHeaders[t]),
                          sizeof(LanimTrackHeader));
            outFile.write(reinterpret_cast<const char*>(trackTimes[t].data()),
                          static_cast<std::streamsize>(trackTimes[t].size() * sizeof(float)));
            outFile.write(reinterpret_cast<const char*>(trackValues[t].data()),
                          static_cast<std::streamsize>(trackValues[t].size() * sizeof(float)));
        }

        std::cout << "  - Animation clip '" << animName << "' -> '" << animPath << "' ("
                  << header.trackCount << " tracks, " << header.duration << "s)" << std::endl;
    }

    return true;
}

// Skinned (.lmesh v2 / LMS2) path: preserves joints/weights through the
// meshoptimizer remap and writes the skeleton block plus sibling.lanim clips.
bool process_skinned_gltf(cgltf_data* data,
                          const std::string& input_path,
                          const std::string& output_path) {
    const cgltf_skin* skin = &data->skins[0];
    if (skin->joints_count == 0 || skin->joints_count > kMaxJointsPerSkeleton) {
        std::cerr << "Error: Skin in " << input_path << " has " << skin->joints_count
                  << " joints (supported: 1.." << kMaxJointsPerSkeleton << ")." << std::endl;
        return false;
    }

    // Map node pointer -> joint index for parent lookups.
    std::unordered_map<const cgltf_node*, int32_t> jointIndexByNode;
    for (size_t j = 0; j < skin->joints_count; ++j) {
        jointIndexByNode.emplace(skin->joints[j], static_cast<int32_t>(j));
    }

    std::vector<Lms2Joint> joints(skin->joints_count);
    for (size_t j = 0; j < skin->joints_count; ++j) {
        const cgltf_node* node = skin->joints[j];
        Lms2Joint& joint = joints[j];
        const size_t nodeIndex = static_cast<size_t>(node - data->nodes);
        joint.nameHash = HashJointName(JointName(node, nodeIndex));
        joint.parentIndex = -1;
        if (node->parent) {
            const auto found = jointIndexByNode.find(node->parent);
            if (found != jointIndexByNode.end())
                joint.parentIndex = found->second;
        }
        if (skin->inverse_bind_matrices) {
            cgltf_accessor_read_float(skin->inverse_bind_matrices, j, joint.inverseBind, 16);
        }
        if (node->has_translation) {
            for (int c = 0; c < 3; ++c)
                joint.localTranslation[c] = node->translation[c];
        }
        if (node->has_rotation) {
            for (int c = 0; c < 4; ++c)
                joint.localRotation[c] = node->rotation[c];
        }
        if (node->has_scale) {
            for (int c = 0; c < 3; ++c)
                joint.localScale[c] = node->scale[c];
        }
    }

    std::vector<SkinnedVertexData> master_raw_vertices;
    std::vector<uint32_t> master_indices;
    size_t vertex_offset = 0;

    for (size_t mesh_idx = 0; mesh_idx < data->meshes_count; ++mesh_idx) {
        for (size_t prim_idx = 0; prim_idx < data->meshes[mesh_idx].primitives_count; ++prim_idx) {
            cgltf_primitive* primitive = &data->meshes[mesh_idx].primitives[prim_idx];

            cgltf_accessor* index_accessor = primitive->indices;
            cgltf_accessor* pos_accessor = nullptr;
            cgltf_accessor* norm_accessor = nullptr;
            cgltf_accessor* uv_accessor = nullptr;
            cgltf_accessor* joints_accessor = nullptr;
            cgltf_accessor* weights_accessor = nullptr;

            for (size_t i = 0; i < primitive->attributes_count; ++i) {
                cgltf_attribute* attr = &primitive->attributes[i];
                if (attr->type == cgltf_attribute_type_position)
                    pos_accessor = attr->data;
                if (attr->type == cgltf_attribute_type_normal)
                    norm_accessor = attr->data;
                if (attr->type == cgltf_attribute_type_texcoord)
                    uv_accessor = attr->data;
                if (attr->type == cgltf_attribute_type_joints && attr->index == 0)
                    joints_accessor = attr->data;
                if (attr->type == cgltf_attribute_type_weights && attr->index == 0)
                    weights_accessor = attr->data;
            }

            if (!index_accessor || !pos_accessor || !norm_accessor || !uv_accessor ||
                !joints_accessor || !weights_accessor) {
                std::cerr << "Warning: Skipping skinned primitive " << prim_idx << " in mesh "
                          << mesh_idx << " due to missing attributes." << std::endl;
                continue;
            }

            for (size_t i = 0; i < index_accessor->count; ++i) {
                master_indices.push_back(
                    static_cast<uint32_t>(cgltf_accessor_read_index(index_accessor, i)) +
                    static_cast<uint32_t>(vertex_offset));
            }

            const size_t current_vertex_count = pos_accessor->count;
            for (size_t v = 0; v < current_vertex_count; ++v) {
                SkinnedVertexData vert{};
                cgltf_accessor_read_float(pos_accessor, v, vert.pos, 3);
                cgltf_accessor_read_float(norm_accessor, v, vert.norm, 3);
                cgltf_accessor_read_float(uv_accessor, v, vert.uv, 2);

                cgltf_uint jointIndices[4] = {0, 0, 0, 0};
                cgltf_accessor_read_uint(joints_accessor, v, jointIndices, 4);
                for (int c = 0; c < 4; ++c) {
                    if (jointIndices[c] >= skin->joints_count) {
                        std::cerr << "Error: Vertex joint index " << jointIndices[c]
                                  << " out of range in " << input_path << std::endl;
                        return false;
                    }
                    vert.joints[c] = static_cast<uint8_t>(jointIndices[c]);
                }

                float rawWeights[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                cgltf_accessor_read_float(weights_accessor, v, rawWeights, 4);
                QuantizeWeights(rawWeights, vert.weights);

                master_raw_vertices.push_back(vert);
            }

            vertex_offset += current_vertex_count;
        }
    }

    if (master_raw_vertices.empty()) {
        std::cerr << "Error: No valid skinned primitives found in " << input_path << std::endl;
        return false;
    }

    // meshoptimizer pipeline with the skinned stride so joints/weights survive
    // the remap untouched.
    std::vector<unsigned int> remap(master_raw_vertices.size());
    size_t unique_vertex_count = meshopt_generateVertexRemap(remap.data(),
                                                             master_indices.data(),
                                                             master_indices.size(),
                                                             master_raw_vertices.data(),
                                                             master_raw_vertices.size(),
                                                             sizeof(SkinnedVertexData));

    std::vector<uint32_t> optimized_indices(master_indices.size());
    meshopt_remapIndexBuffer(
        optimized_indices.data(), master_indices.data(), master_indices.size(), remap.data());

    std::vector<SkinnedVertexData> optimized_vertices(unique_vertex_count);
    meshopt_remapVertexBuffer(optimized_vertices.data(),
                              master_raw_vertices.data(),
                              master_raw_vertices.size(),
                              sizeof(SkinnedVertexData),
                              remap.data());

    meshopt_optimizeVertexCache(optimized_indices.data(),
                                optimized_indices.data(),
                                optimized_indices.size(),
                                unique_vertex_count);
    meshopt_optimizeOverdraw(optimized_indices.data(),
                             optimized_indices.data(),
                             optimized_indices.size(),
                             &optimized_vertices[0].pos[0],
                             unique_vertex_count,
                             sizeof(SkinnedVertexData),
                             1.05f);
    meshopt_optimizeVertexFetch(optimized_vertices.data(),
                                optimized_indices.data(),
                                optimized_indices.size(),
                                optimized_vertices.data(),
                                unique_vertex_count,
                                sizeof(SkinnedVertexData));

    float min_ext[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float max_ext[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (const auto& v : optimized_vertices) {
        for (int c = 0; c < 3; ++c) {
            min_ext[c] = std::min(min_ext[c], v.pos[c]);
            max_ext[c] = std::max(max_ext[c], v.pos[c]);
        }
    }

    Lms2Header header;
    header.vertexCount = static_cast<uint32_t>(unique_vertex_count);
    header.indexCount = static_cast<uint32_t>(optimized_indices.size());
    header.jointCount = static_cast<uint32_t>(joints.size());
    header.boundingSphere[0] = (min_ext[0] + max_ext[0]) / 2.0f;
    header.boundingSphere[1] = (min_ext[1] + max_ext[1]) / 2.0f;
    header.boundingSphere[2] = (min_ext[2] + max_ext[2]) / 2.0f;
    const float dx = max_ext[0] - header.boundingSphere[0];
    const float dy = max_ext[1] - header.boundingSphere[1];
    const float dz = max_ext[2] - header.boundingSphere[2];
    header.boundingSphere[3] = sqrtf(dx * dx + dy * dy + dz * dz);

    std::ofstream outFile(output_path, std::ios::binary);
    if (!outFile) {
        std::cerr << "Error: Could not open output file " << output_path << std::endl;
        return false;
    }

    outFile.write(reinterpret_cast<const char*>(&header), sizeof(header));
    outFile.write(reinterpret_cast<const char*>(optimized_vertices.data()),
                  static_cast<std::streamsize>(unique_vertex_count * sizeof(SkinnedVertexData)));
    outFile.write(reinterpret_cast<const char*>(optimized_indices.data()),
                  static_cast<std::streamsize>(optimized_indices.size() * sizeof(uint32_t)));
    outFile.write(reinterpret_cast<const char*>(joints.data()),
                  static_cast<std::streamsize>(joints.size() * sizeof(Lms2Joint)));
    outFile.close();

    std::cout << "Successfully processed skinned '" << input_path << "' -> '" << output_path
              << "' (LMS2)" << std::endl;
    std::cout << "  - Vertices: " << master_raw_vertices.size() << " -> " << unique_vertex_count
              << std::endl;
    std::cout << "  - Indices: " << optimized_indices.size() << std::endl;
    std::cout << "  - Joints: " << joints.size() << std::endl;

    return static_cast<bool>(outFile) && WriteAnimationClips(data, output_path);
}

// ----------------------------------------------------------------------------
// Texture import: PNG -> .ltex
//
//.ltex is a minimal, self-describing mip-chained 8-bit texture container. The
// header is written field-by-field (no struct padding) so the on-disk layout
// is unambiguous and trivially round-trippable:
//
//   u32 magic    = 'LTEX' little-endian (0x5845544C)
//   u16 version  = 1
//   u16 mip_count
//   u32 width    (mip 0)
//   u32 height   (mip 0)
//   u8  channels (1..4)
//   <raw mip chain>: for each level, width*height*channels bytes, dimensions
//                     halve (floored, min 1) per level, box-filtered from the
//                     previous level. Layout matches glTexSubImage3D upload.
//
// The engine's public texture ABI is .ltex; KTX2 is intentionally outside the
// supported importer and runtime formats.
// ----------------------------------------------------------------------------

constexpr uint32_t kLtexMagic = 0x5845544C; // 'LTEX' little-endian
constexpr uint16_t kLtexVersion = 1;

struct LtexMip {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels; // width * height * channels
};

// Box-filter downsample by 2 (floored, min 1). Averages the up-to-4 source
// texels covering each destination texel; deterministic integer rounding.
LtexMip BoxFilterDownsample(const LtexMip& src, uint32_t channels) {
    LtexMip dst;
    dst.width = std::max(1u, src.width / 2u);
    dst.height = std::max(1u, src.height / 2u);
    dst.pixels.resize(static_cast<size_t>(dst.width) * dst.height * channels);

    for (uint32_t y = 0; y < dst.height; ++y) {
        const uint32_t sy0 = std::min(y * 2u, src.height - 1u);
        const uint32_t sy1 = std::min(sy0 + 1u, src.height - 1u);
        for (uint32_t x = 0; x < dst.width; ++x) {
            const uint32_t sx0 = std::min(x * 2u, src.width - 1u);
            const uint32_t sx1 = std::min(sx0 + 1u, src.width - 1u);
            for (uint32_t c = 0; c < channels; ++c) {
                auto at = [&](uint32_t px, uint32_t py) -> uint32_t {
                    return src.pixels[(static_cast<size_t>(py) * src.width + px) * channels + c];
                };
                const uint32_t sum = at(sx0, sy0) + at(sx1, sy0) + at(sx0, sy1) + at(sx1, sy1);
                // Round-to-nearest of the 4-texel average.
                dst.pixels[(static_cast<size_t>(y) * dst.width + x) * channels + c] =
                    static_cast<uint8_t>((sum + 2u) / 4u);
            }
        }
    }
    return dst;
}

// Build the full mip chain down to 1x1.
std::vector<LtexMip> BuildMipChain(LtexMip base, uint32_t channels) {
    std::vector<LtexMip> chain;
    chain.push_back(std::move(base));
    while (chain.back().width > 1u || chain.back().height > 1u) {
        chain.push_back(BoxFilterDownsample(chain.back(), channels));
    }
    return chain;
}

bool WriteLtex(const std::string& output_path,
               uint32_t width,
               uint32_t height,
               uint32_t channels,
               const std::vector<LtexMip>& mips) {
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        std::cerr << "Error: Could not open output file " << output_path << std::endl;
        return false;
    }

    const uint16_t mip_count = static_cast<uint16_t>(mips.size());
    const uint8_t channels8 = static_cast<uint8_t>(channels);

    out.write(reinterpret_cast<const char*>(&kLtexMagic), sizeof(kLtexMagic));
    out.write(reinterpret_cast<const char*>(&kLtexVersion), sizeof(kLtexVersion));
    out.write(reinterpret_cast<const char*>(&mip_count), sizeof(mip_count));
    out.write(reinterpret_cast<const char*>(&width), sizeof(width));
    out.write(reinterpret_cast<const char*>(&height), sizeof(height));
    out.write(reinterpret_cast<const char*>(&channels8), sizeof(channels8));

    for (const LtexMip& mip : mips) {
        out.write(reinterpret_cast<const char*>(mip.pixels.data()),
                  static_cast<std::streamsize>(mip.pixels.size()));
    }
    return static_cast<bool>(out);
}

} // namespace

// Public entry point (also used by the round-trip test, which re-declares this
// signature). Imports a PNG and writes a box-filter mip-chained.ltex.
//
// target_size: when > 0 the source is resized to target_size x
// target_size (sRGB-correct box/Mitchell resample via stb_image_resize2) before
// the mip chain is built. Terrain albedo/normal source plates are 2K PBR maps;
// the committed  terrain arrays are 256x256 so the whole terrain set
// stays well inside the 96 MB residency budget. When target_size == 0 the source
// is imported at its native size (the  round-trip behaviour, byte-stable).
//
// emit_preview_png: when true a sibling <stem>.png is written next to
// the.ltex holding the (possibly resized) mip-0 image, so the downsized terrain
// plate is committable/reviewable as a PNG alongside the binary.ltex.
// 4-arg worker. The 2-arg public overload below preserves the
// ABI/declaration the round-trip test links against.
bool process_material_texture(const std::string& input_path,
                              const std::string& output_path,
                              uint32_t target_size,
                              bool emit_preview_png,
                              bool normal_map,
                              const std::string& alpha_mask,
                              bool material_mips,
                              bool linear_data) {
    int width = 0;
    int height = 0;
    int source_channels = 0;
    // Do NOT flip:.ltex stores PNG row order; the runtime upload path applies
    // any flip it needs. Force 4 channels (RGBA) for the texture-array path.
    constexpr int kForcedChannels = 4;
    stbi_set_flip_vertically_on_load(false);
    unsigned char* data =
        stbi_load(input_path.c_str(), &width, &height, &source_channels, kForcedChannels);
    if (!data) {
        std::cerr << "Error: Could not load image: " << input_path << " (" << stbi_failure_reason()
                  << ")" << std::endl;
        return false;
    }
    if (width <= 0 || height <= 0) {
        std::cerr << "Error: Image has invalid dimensions: " << input_path << std::endl;
        stbi_image_free(data);
        return false;
    }
    if (!alpha_mask.empty()) {
        int mask_width = 0, mask_height = 0, mask_channels = 0;
        unsigned char* mask =
            stbi_load(alpha_mask.c_str(), &mask_width, &mask_height, &mask_channels, 1);
        if (!mask || mask_width != width || mask_height != height || normal_map) {
            std::cerr << "Error: Alpha mask must match the albedo dimensions: " << alpha_mask
                      << std::endl;
            stbi_image_free(mask);
            stbi_image_free(data);
            return false;
        }
        for (size_t p = 0; p < static_cast<size_t>(width) * height; ++p)
            data[p * 4 + 3] = mask[p];
        stbi_image_free(mask);
    }

    const uint32_t channels = static_cast<uint32_t>(kForcedChannels);
    LtexMip base;

    if (target_size > 0 && (static_cast<uint32_t>(width) != target_size ||
                            static_cast<uint32_t>(height) != target_size)) {
        std::vector<uint8_t> resized(static_cast<size_t>(target_size) * target_size * channels);
        const auto resize =
            (normal_map || linear_data) ? stbir_resize_uint8_linear : stbir_resize_uint8_srgb;
        unsigned char* out = resize(data,
                                    width,
                                    height,
                                    0,
                                    resized.data(),
                                    static_cast<int>(target_size),
                                    static_cast<int>(target_size),
                                    0,
                                    STBIR_RGBA);
        if (!out) {
            std::cerr << "Error: Could not resize image: " << input_path << std::endl;
            stbi_image_free(data);
            return false;
        }
        base.width = target_size;
        base.height = target_size;
        base.pixels = std::move(resized);
        width = static_cast<int>(target_size);
        height = static_cast<int>(target_size);
    } else {
        base.width = static_cast<uint32_t>(width);
        base.height = static_cast<uint32_t>(height);
        base.pixels.assign(data, data + static_cast<size_t>(width) * height * channels);
    }
    stbi_image_free(data);

    auto normalize_normals = [](LtexMip& mip) {
        for (size_t p = 0; p < mip.pixels.size(); p += 4) {
            float n[3];
            float length_squared = 0.0f;
            for (int c = 0; c < 3; ++c) {
                n[c] = mip.pixels[p + c] / 127.5f - 1.0f;
                length_squared += n[c] * n[c];
            }
            const float inverse_length =
                length_squared > 1e-8f ? 1.0f / std::sqrt(length_squared) : 0.0f;
            for (int c = 0; c < 3; ++c) {
                const float component =
                    inverse_length > 0.0f ? n[c] * inverse_length : (c == 2 ? 1.0f : 0.0f);
                mip.pixels[p + c] = static_cast<uint8_t>(
                    std::clamp(std::lround((component + 1.0f) * 127.5f), 0l, 255l));
            }
        }
    };
    if (normal_map)
        normalize_normals(base);

    if (emit_preview_png) {
        const std::string preview = OutputStem(output_path) + ".png";
        if (!stbi_write_png(preview.c_str(),
                            width,
                            height,
                            static_cast<int>(channels),
                            base.pixels.data(),
                            width * static_cast<int>(channels))) {
            std::cerr << "Warning: Could not write preview PNG: " << preview << std::endl;
        } else {
            std::cout << "  - Preview PNG: " << preview << std::endl;
        }
    }

    std::vector<LtexMip> mips;
    if (!normal_map && alpha_mask.empty() && !material_mips && !linear_data) {
        // Preserve the existing native texture import ABI and its byte-level fixtures.
        mips = BuildMipChain(std::move(base), channels);
    } else {
        auto coverage = [](const LtexMip& mip, float scale) {
            size_t covered = 0;
            for (size_t p = 3; p < mip.pixels.size(); p += 4)
                covered += mip.pixels[p] * scale >= 127.5f;
            return static_cast<float>(covered) / static_cast<float>(mip.width * mip.height);
        };
        const float base_coverage = coverage(base, 1.0f);
        mips.push_back(std::move(base));
        while (mips.back().width > 1 || mips.back().height > 1) {
            const auto& source = mips.back();
            LtexMip mip;
            mip.width = std::max(1u, source.width / 2);
            mip.height = std::max(1u, source.height / 2);
            mip.pixels.resize(static_cast<size_t>(mip.width) * mip.height * channels);
            const auto resize =
                (normal_map || linear_data) ? stbir_resize_uint8_linear : stbir_resize_uint8_srgb;
            if (!resize(source.pixels.data(),
                        static_cast<int>(source.width),
                        static_cast<int>(source.height),
                        0,
                        mip.pixels.data(),
                        static_cast<int>(mip.width),
                        static_cast<int>(mip.height),
                        0,
                        STBIR_RGBA))
                return false;
            if (normal_map) {
                normalize_normals(mip);
            } else if (!alpha_mask.empty()) {
                // Preserve leaf coverage at the runtime alpha cutoff (0.5) as cards recede.
                float lo = 0.0f, hi = 256.0f;
                for (int iteration = 0; iteration < 16; ++iteration) {
                    const float mid = (lo + hi) * 0.5f;
                    if (coverage(mip, mid) < base_coverage)
                        lo = mid;
                    else
                        hi = mid;
                }
                const float scale = std::abs(coverage(mip, lo) - base_coverage) <
                                            std::abs(coverage(mip, hi) - base_coverage)
                                        ? lo
                                        : hi;
                for (size_t p = 3; p < mip.pixels.size(); p += 4)
                    mip.pixels[p] = static_cast<uint8_t>(
                        std::clamp(std::lround(mip.pixels[p] * scale), 0l, 255l));
            }
            mips.push_back(std::move(mip));
        }
    }
    if (!WriteLtex(output_path,
                   static_cast<uint32_t>(width),
                   static_cast<uint32_t>(height),
                   channels,
                   mips)) {
        return false;
    }

    std::cout << "Successfully processed texture '" << input_path << "' -> '" << output_path
              << "' (LTEX)" << std::endl;
    std::cout << "  - Dimensions: " << width << "x" << height << ", channels: " << channels
              << std::endl;
    std::cout << "  - Mip levels: " << mips.size() << std::endl;
    return true;
}

bool process_texture_resized(const std::string& input_path,
                             const std::string& output_path,
                             uint32_t target_size,
                             bool emit_preview_png) {
    return process_material_texture(
        input_path, output_path, target_size, emit_preview_png, false, {}, false, false);
}

//  ABI: native-size PNG ->.ltex import. Stable signature the round-trip
// test re-declares and links against.
bool process_texture(const std::string& input_path, const std::string& output_path) {
    return process_texture_resized(input_path, output_path, 0, false);
}

namespace {

using ImportMatrix = std::array<float, 16>;
constexpr ImportMatrix kImportIdentity = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

struct StaticInstance {
    const cgltf_mesh* mesh = nullptr;
    ImportMatrix world = kImportIdentity;
    size_t nodeIndex = SIZE_MAX; // No node for a mesh-only library document.
};

bool ImportError(const char* rule, const std::string& message) {
    std::cerr << "Error [" << rule << "]: " << message << '\n';
    return false;
}

bool FiniteAffine(const ImportMatrix& matrix) {
    return std::all_of(matrix.begin(), matrix.end(), [](float v) { return std::isfinite(v); }) &&
           matrix[3] == 0 && matrix[7] == 0 && matrix[11] == 0 && matrix[15] == 1;
}

bool CollectStaticInstances(const cgltf_data* data, std::vector<StaticInstance>& instances) {
    for (size_t i = 0; i < data->extensions_required_count; ++i) {
        if (std::strcmp(data->extensions_required[i], "KHR_texture_transform") != 0)
            return ImportError("extension.unsupported", data->extensions_required[i]);
    }
    if (data->animations_count != 0)
        return ImportError("static.animation",
                           "Bake a selected static pose before mesh conversion.");

    const cgltf_scene* scene = data->scene;
    if (!scene && data->scenes_count > 1)
        return ImportError("scene.ambiguous",
                           "Select a default scene before exporting multiple scenes.");
    if (!scene && data->scenes_count == 1)
        scene = &data->scenes[0];

    struct Pending {
        const cgltf_node* node;
        ImportMatrix parent;
    };
    std::vector<Pending> pending;
    if (scene) {
        for (size_t i = scene->nodes_count; i > 0; --i)
            pending.push_back({scene->nodes[i - 1], kImportIdentity});
    } else if (data->nodes_count) {
        for (size_t i = data->nodes_count; i > 0; --i) {
            if (!data->nodes[i - 1].parent)
                pending.push_back({&data->nodes[i - 1], kImportIdentity});
        }
    } else {
        // Preserve the legacy library path only when there are no scene nodes
        // whose placement or membership could be discarded.
        for (size_t i = 0; i < data->meshes_count; ++i)
            instances.push_back({&data->meshes[i], kImportIdentity, SIZE_MAX});
    }

    std::vector<bool> visited(data->nodes_count, false);
    while (!pending.empty()) {
        const Pending entry = pending.back();
        pending.pop_back();
        const auto* node = entry.node;
        const size_t index = static_cast<size_t>(node - data->nodes);
        if (visited[index])
            return ImportError("scene.hierarchy",
                               "Repeated or cyclic node " + std::to_string(index));
        visited[index] = true;
        if (node->has_mesh_gpu_instancing)
            return ImportError("scene.gpu_instances",
                               "Realize GPU instances on node " + std::to_string(index));
        if (node->has_matrix && (node->has_translation || node->has_rotation || node->has_scale))
            return ImportError("scene.transform",
                               "Use matrix or TRS, not both, on node " + std::to_string(index));
        ImportMatrix local;
        cgltf_node_transform_local(node, local.data());
        if (!FiniteAffine(local))
            return ImportError("scene.transform",
                               "Expected finite affine transform on node " + std::to_string(index));
        ImportMatrix world{};
        for (size_t col = 0; col < 4; ++col) {
            for (size_t row = 0; row < 4; ++row) {
                for (size_t k = 0; k < 4; ++k)
                    world[col * 4 + row] += entry.parent[k * 4 + row] * local[col * 4 + k];
            }
        }
        if (!FiniteAffine(world))
            return ImportError("scene.transform",
                               "World transform overflows on node " + std::to_string(index));
        if (node->mesh)
            instances.push_back({node->mesh, world, index});
        for (size_t i = node->children_count; i > 0; --i)
            pending.push_back({node->children[i - 1], world});
    }
    return true;
}

class StaticTransform {
    const ImportMatrix& matrix;
    double cofactors[9]{};
    double determinant = 0;

public:
    explicit StaticTransform(const ImportMatrix& m)
        : matrix(m) {
        for (size_t col = 0; col < 3; ++col) {
            const size_t a = ((col + 1) % 3) * 4;
            const size_t b = ((col + 2) % 3) * 4;
            cofactors[col * 3] = double(m[a + 1]) * m[b + 2] - double(m[a + 2]) * m[b + 1];
            cofactors[col * 3 + 1] = double(m[a + 2]) * m[b] - double(m[a]) * m[b + 2];
            cofactors[col * 3 + 2] = double(m[a]) * m[b + 1] - double(m[a + 1]) * m[b];
        }
        determinant = m[0] * cofactors[0] + m[1] * cofactors[1] + m[2] * cofactors[2];
    }

    bool Apply(Vertex& vertex) const {
        for (const float v : vertex.pos)
            if (!std::isfinite(v))
                return false;
        for (const float v : vertex.norm)
            if (!std::isfinite(v))
                return false;
        for (const float v : vertex.uv)
            if (!std::isfinite(v))
                return false;
        double length = 0;
        for (const float v : vertex.norm)
            length += double(v) * v;
        if (length == 0)
            return false;
        // Avoid changing previously supported identity geometry's exact bytes.
        if (matrix == kImportIdentity)
            return true;
        const Vertex source = vertex;
        double normal[3]{};
        length = 0;
        for (size_t row = 0; row < 3; ++row) {
            double position = matrix[12 + row];
            for (size_t col = 0; col < 3; ++col) {
                position += double(matrix[col * 4 + row]) * source.pos[col];
                normal[row] += cofactors[col * 3 + row] * source.norm[col] / determinant;
            }
            vertex.pos[row] = static_cast<float>(position);
            if (!std::isfinite(vertex.pos[row]))
                return false;
            length += normal[row] * normal[row];
        }
        if (!std::isfinite(length) || length == 0)
            return false;
        const double inverseLength = 1 / std::sqrt(length);
        for (size_t row = 0; row < 3; ++row)
            vertex.norm[row] = static_cast<float>(normal[row] * inverseLength);
        return true;
    }

    bool IsSingular() const {
        return determinant == 0 || !std::isfinite(determinant);
    }

    bool IsMirrored() const {
        return determinant < 0;
    }
};

bool AppendStaticPrimitive(const cgltf_primitive& primitive,
                           const StaticInstance& instance,
                           size_t primitiveIndex,
                           std::vector<Vertex>& vertices,
                           std::vector<uint32_t>& indices) {
    const std::string owner = instance.nodeIndex == SIZE_MAX
                                  ? "mesh library"
                                  : "node " + std::to_string(instance.nodeIndex);
    const std::string location = owner + ", primitive " + std::to_string(primitiveIndex) + ": ";
    const auto fail = [&](const char* rule, const char* message) {
        return ImportError(rule, location + message);
    };
    if (primitive.type != cgltf_primitive_type_triangles)
        return fail("mesh.topology", "Export indexed triangles.");
    if (primitive.targets_count)
        return fail("mesh.morph_targets", "Bake shape keys before static mesh conversion.");
    const StaticTransform transform(instance.world);
    if (transform.IsSingular())
        return fail("scene.singular_transform", "Remove zero-scale or singular transforms.");
    const cgltf_texture_view* color = nullptr;
    int wantedUv = 0;
    if (primitive.material && primitive.material->has_pbr_metallic_roughness) {
        color = &primitive.material->pbr_metallic_roughness.base_color_texture;
        wantedUv = color->has_transform && color->transform.has_texcoord ? color->transform.texcoord
                                                                         : color->texcoord;
    }
    const cgltf_accessor *position = nullptr, *normal = nullptr, *uv = nullptr;
    for (size_t i = 0; i < primitive.attributes_count; ++i) {
        const auto& attr = primitive.attributes[i];
        if (attr.type == cgltf_attribute_type_position)
            position = attr.data;
        if (attr.type == cgltf_attribute_type_normal)
            normal = attr.data;
        if (attr.type == cgltf_attribute_type_texcoord && attr.index == wantedUv)
            uv = attr.data;
    }
    const auto* index = primitive.indices;
    if (!position || !normal || !uv || !index)
        return fail("mesh.attributes",
                    "Supply POSITION, NORMAL, the material's TEXCOORD set, and indices.");
    if (position->is_sparse || normal->is_sparse || uv->is_sparse || index->is_sparse)
        return fail("mesh.sparse_accessor",
                    "Expand sparse attributes and indices before conversion.");
    if (position->type != cgltf_type_vec3 || normal->type != cgltf_type_vec3 ||
        uv->type != cgltf_type_vec2 || position->count == 0 || normal->count != position->count ||
        uv->count != position->count)
        return fail("mesh.attributes", "Attribute types and vertex counts must agree.");
    if (index->count == 0 || index->count % 3 != 0)
        return fail("mesh.indices", "Triangle index count must be a nonzero multiple of three.");
    const size_t offset = vertices.size();
    if (offset > UINT32_MAX || position->count > UINT32_MAX - offset ||
        indices.size() > UINT32_MAX || index->count > UINT32_MAX - indices.size())
        return fail("mesh.limit", "Geometry exceeds the 32-bit mesh format.");
    for (size_t i = 0; i < position->count; ++i) {
        Vertex vertex{};
        if (!cgltf_accessor_read_float(position, i, vertex.pos, 3) ||
            !cgltf_accessor_read_float(normal, i, vertex.norm, 3) ||
            !cgltf_accessor_read_float(uv, i, vertex.uv, 2))
            return fail("mesh.accessor", "Cannot decode a required vertex attribute.");
        if (color && color->has_transform) {
            const auto& t = color->transform;
            const float u = vertex.uv[0] * t.scale[0];
            const float v = vertex.uv[1] * t.scale[1];
            const float c = std::cos(t.rotation), s = std::sin(t.rotation);
            vertex.uv[0] = c * u - s * v + t.offset[0];
            vertex.uv[1] = s * u + c * v + t.offset[1];
        }
        if (!transform.Apply(vertex))
            return fail(
                "mesh.nonfinite",
                "Export finite attributes and nonzero normals within representable bounds.");
        vertices.push_back(vertex);
    }
    for (size_t i = 0; i < index->count; i += 3) {
        uint32_t triangle[3];
        for (size_t lane = 0; lane < 3; ++lane) {
            const size_t local = cgltf_accessor_read_index(index, i + lane);
            if (local >= position->count)
                return fail("mesh.indices", "Index is outside the primitive's vertex range.");
            triangle[lane] = static_cast<uint32_t>(local + offset);
        }
        if (transform.IsMirrored())
            std::swap(triangle[1], triangle[2]);
        indices.insert(indices.end(), std::begin(triangle), std::end(triangle));
    }
    return true;
}

} // namespace

// optional triangle budget for the static (.lmesh) path, set by main from
// --max-tris. A file-static keeps process_gltf's 2-arg signature intact so the
// asset round-trip tests (which forward-declare process_gltf(string,string)) and
// the CMake auto-asset rule keep linking unchanged. 0 = no decimation.
static size_t g_max_tris = 0;
// when >= 0, export ONLY this global primitive index (counted across all
// meshes) instead of merging every primitive. Multi-material assets (e.g. the
// tree: trunk/branch/leaves, each with its OWN atlas + UV set) are split into
// one.lmesh per part so each part samples its correct texture via the static-
// model UV lane. -1 = merge all (default, unchanged behaviour).
static int g_only_primitive = -1;

bool process_gltf_checked(const std::string& input_path, const std::string& output_path) {
    cgltf_options options = {};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, input_path.c_str(), &data) != cgltf_result_success) {
        std::cerr << "Error: Could not parse glTF file: " << input_path << std::endl;
        return false;
    }

    if (cgltf_load_buffers(&options, data, input_path.c_str()) != cgltf_result_success) {
        std::cerr << "Error: Could not load glTF buffers for: " << input_path << std::endl;
        cgltf_free(data);
        return false;
    }

    if (cgltf_validate(data) != cgltf_result_success) {
        std::cerr << "Error: Invalid glTF data: " << input_path << std::endl;
        cgltf_free(data);
        return false;
    }

    // Skinned assets take the LMS2 path; unskinned input continues through the
    // LMSH writer below. Identity geometry retains its existing byte layout;
    // scene instances and transforms are now baked into the static geometry.
    if (data->skins_count > 0) {
        const bool ok = process_skinned_gltf(data, input_path, output_path);
        cgltf_free(data);
        return ok;
    }

    std::vector<StaticInstance> instances;
    if (!CollectStaticInstances(data, instances)) {
        cgltf_free(data);
        return false;
    }
    std::vector<Vertex> master_raw_vertices;
    std::vector<uint32_t> master_indices;
    // --primitive keeps identifying a source mesh primitive, independently of
    // node traversal order. Every selected instance of that primitive is baked.
    std::vector<size_t> primitiveOffsets(data->meshes_count, 0);
    for (size_t i = 1; i < data->meshes_count; ++i)
        primitiveOffsets[i] = primitiveOffsets[i - 1] + data->meshes[i - 1].primitives_count;
    for (const auto& instance : instances) {
        const size_t meshIndex = static_cast<size_t>(instance.mesh - data->meshes);
        for (size_t i = 0; i < instance.mesh->primitives_count; ++i) {
            const size_t primitiveIndex = primitiveOffsets[meshIndex] + i;
            if (g_only_primitive >= 0 && primitiveIndex != static_cast<size_t>(g_only_primitive))
                continue;
            if (!AppendStaticPrimitive(instance.mesh->primitives[i],
                                       instance,
                                       primitiveIndex,
                                       master_raw_vertices,
                                       master_indices)) {
                cgltf_free(data);
                return false;
            }
        }
    }

    if (master_raw_vertices.empty()) {
        std::cerr << "Error: No valid primitives found in " << input_path << std::endl;
        cgltf_free(data);
        return false;
    }

    // Now, run the optimization pipeline on the combined geometry
    std::vector<unsigned int> remap(master_raw_vertices.size());
    size_t unique_vertex_count = meshopt_generateVertexRemap(remap.data(),
                                                             master_indices.data(),
                                                             master_indices.size(),
                                                             master_raw_vertices.data(),
                                                             master_raw_vertices.size(),
                                                             sizeof(Vertex));

    std::vector<uint32_t> optimized_indices(master_indices.size());
    meshopt_remapIndexBuffer(
        optimized_indices.data(), master_indices.data(), master_indices.size(), remap.data());

    std::vector<Vertex> optimized_vertices(unique_vertex_count);
    meshopt_remapVertexBuffer(optimized_vertices.data(),
                              master_raw_vertices.data(),
                              master_raw_vertices.size(),
                              sizeof(Vertex),
                              remap.data());

    meshopt_optimizeVertexCache(optimized_indices.data(),
                                optimized_indices.data(),
                                optimized_indices.size(),
                                unique_vertex_count);
    meshopt_optimizeOverdraw(optimized_indices.data(),
                             optimized_indices.data(),
                             optimized_indices.size(),
                             &optimized_vertices[0].pos[0],
                             unique_vertex_count,
                             sizeof(Vertex),
                             1.05f);
    meshopt_optimizeVertexFetch(optimized_vertices.data(),
                                optimized_indices.data(),
                                optimized_indices.size(),
                                optimized_vertices.data(),
                                unique_vertex_count,
                                sizeof(Vertex));

    // optional decimation to a triangle budget (LOD0). Photogrammetry/SpeedTree
    // exports can be millions of tris (tree_small_02 = ~2.06M); at instance scale
    // that is a vertex/overdraw bomb with no LOD. meshopt_simplify collapses toward
    // the target while bounding the geometric error and preserving the UV seams;
    // we then re-optimize and COMPACT the vertex buffer so the written mesh is tight.
    if (g_max_tris > 0 && optimized_indices.size() / 3 > g_max_tris) {
        const size_t target_index_count = g_max_tris * 3;
        std::vector<unsigned int> simplified(optimized_indices.size());
        float result_error = 0.0f;
        // Sparse computes relative error against the referenced subset extents.
        // Preserve disconnected components. Topology and error limits may stop
        // above the target; record actual counts instead of pruning canopy coverage.
        const size_t simplified_count = meshopt_simplify(simplified.data(),
                                                         optimized_indices.data(),
                                                         optimized_indices.size(),
                                                         &optimized_vertices[0].pos[0],
                                                         unique_vertex_count,
                                                         sizeof(Vertex),
                                                         target_index_count,
                                                         /*target_error*/ 0.10f,
                                                         meshopt_SimplifySparse,
                                                         &result_error);
        simplified.resize(simplified_count);
        optimized_indices.swap(simplified);
        meshopt_optimizeVertexCache(optimized_indices.data(),
                                    optimized_indices.data(),
                                    optimized_indices.size(),
                                    unique_vertex_count);
        const size_t new_vertex_count = meshopt_optimizeVertexFetch(optimized_vertices.data(),
                                                                    optimized_indices.data(),
                                                                    optimized_indices.size(),
                                                                    optimized_vertices.data(),
                                                                    unique_vertex_count,
                                                                    sizeof(Vertex));
        optimized_vertices.resize(new_vertex_count);
        unique_vertex_count = new_vertex_count;
        std::cout << "  - Simplified to " << (optimized_indices.size() / 3) << " tris (target "
                  << g_max_tris << ", rel error " << result_error << ")" << std::endl;
    }

    float min_ext[3] = {FLT_MAX, FLT_MAX, FLT_MAX};
    float max_ext[3] = {-FLT_MAX, -FLT_MAX, -FLT_MAX};

    for (const auto& v : optimized_vertices) {
        min_ext[0] = std::min(min_ext[0], v.pos[0]);
        min_ext[1] = std::min(min_ext[1], v.pos[1]);
        min_ext[2] = std::min(min_ext[2], v.pos[2]);
        max_ext[0] = std::max(max_ext[0], v.pos[0]);
        max_ext[1] = std::max(max_ext[1], v.pos[1]);
        max_ext[2] = std::max(max_ext[2], v.pos[2]);
    }

    float sphere[4];
    sphere[0] = (min_ext[0] + max_ext[0]) / 2.0f;
    sphere[1] = (min_ext[1] + max_ext[1]) / 2.0f;
    sphere[2] = (min_ext[2] + max_ext[2]) / 2.0f;

    float dx = max_ext[0] - sphere[0];
    float dy = max_ext[1] - sphere[1];
    float dz = max_ext[2] - sphere[2];
    sphere[3] = sqrtf(dx * dx + dy * dy + dz * dz);

    if (!std::all_of(
            std::begin(sphere), std::end(sphere), [](float v) { return std::isfinite(v); })) {
        cgltf_free(data);
        return ImportError("mesh.bounds",
                           "Compiled bounding sphere exceeds the finite mesh format.");
    }

    std::ofstream outFile(output_path, std::ios::binary);
    if (!outFile) {
        std::cerr << "Error: Could not open output file " << output_path << std::endl;
        cgltf_free(data);
        return false;
    }

    LMeshHeader header;
    header.magic = *reinterpret_cast<const uint32_t*>("LMSH");
    header.vertexCount = (uint32_t)unique_vertex_count;
    header.indexCount = (uint32_t)optimized_indices.size();
    memcpy(header.boundingSphere, sphere, sizeof(sphere));

    outFile.write(reinterpret_cast<const char*>(&header), sizeof(LMeshHeader));
    outFile.write(reinterpret_cast<const char*>(optimized_vertices.data()),
                  unique_vertex_count * sizeof(Vertex));
    outFile.write(reinterpret_cast<const char*>(optimized_indices.data()),
                  optimized_indices.size() * sizeof(uint32_t));
    outFile.close();

    std::cout << "Successfully processed '" << input_path << "' -> '" << output_path << "'"
              << std::endl;
    std::cout << "  - Vertices: " << master_raw_vertices.size() << " -> " << unique_vertex_count
              << std::endl;
    std::cout << "  - Indices: " << optimized_indices.size() << std::endl;

    cgltf_free(data);
    return static_cast<bool>(outFile);
}

void process_gltf(const std::string& input_path, const std::string& output_path) {
    (void)process_gltf_checked(input_path, output_path);
}

int main(int argc, char* argv[]) {
    g_only_primitive = -1;
    g_max_tris = 0;
    if (argc < 3) {
        std::cerr << "Usage: AssetProcessor.exe <input.glb|input.png> <output.lmesh|output.ltex> "
                     "[target_size] [--preview-png] [--max-tris N]"
                  << std::endl;
        std::cerr << "  --max-tris N: for.lmesh output, simplify toward N triangles, bounded by "
                     "topology and error (LOD0)"
                  << std::endl;
        std::cerr << "  --emit-lods: for.lmesh output, also write coarser <stem>.lod1.lmesh /"
                     " <stem>.lod2.lmesh distance LODs (render-only; renderer falls back to LOD0"
                     " if absent)"
                  << std::endl;
        std::cerr
            << "  --primitive N: for.lmesh output, export ONLY global primitive N (per-part split"
               " of a multi-material asset) using that part's own UV set"
            << std::endl;
        std::cerr << "  target_size: for.ltex output, resize the source to N x N before mipping "
                     "(0 / omitted = native)"
                  << std::endl;
        std::cerr << "  --preview-png: for.ltex output, also write a sibling <stem>.png of the "
                     "(resized) mip-0 image"
                  << std::endl;
        std::cerr << "  --srgb: filter color mips in linear light\n"
                     "  --linear: filter non-color data without gamma conversion\n"
                     "  --normal-map: filter linear normals and renormalize\n"
                     "  --alpha-mask <image>: merge an opacity map and preserve cutout coverage\n";
        return 1;
    }

    const std::string output_path = argv[2];
    auto ends_with = [](const std::string& s, const std::string& suffix) {
        if (s.size() < suffix.size())
            return false;
        return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(), [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    };

    if (ends_with(output_path, ".ltex")) {
        uint32_t target_size = 0;
        bool emit_preview_png = false;
        bool normal_map = false;
        bool material_mips = false;
        bool linear_data = false;
        std::string alpha_mask;
        for (int a = 3; a < argc; ++a) {
            const std::string arg = argv[a];
            if (arg == "--preview-png") {
                emit_preview_png = true;
            } else if (arg == "--linear") {
                linear_data = true;
            } else if (arg == "--srgb") {
                material_mips = true;
            } else if (arg == "--normal-map") {
                normal_map = true;
            } else if (arg == "--alpha-mask" && a + 1 < argc) {
                alpha_mask = argv[++a];
            } else {
                try {
                    target_size = static_cast<uint32_t>(std::stoul(arg));
                } catch (...) {
                    std::cerr << "Error: unrecognized argument '" << arg << "'" << std::endl;
                    return 1;
                }
            }
        }
        if (target_size > 16384) {
            std::cerr << "Error: target size exceeds 16384" << std::endl;
            return 1;
        }
        return process_material_texture(argv[1],
                                        output_path,
                                        target_size,
                                        emit_preview_png,
                                        normal_map,
                                        alpha_mask,
                                        material_mips,
                                        linear_data)
                   ? 0
                   : 1;
    }

    // optional triangle budget for the static (.lmesh) path. --max-tris N
    // decimates the combined mesh to <= N triangles (LOD0). Used to bring
    // photogrammetry/SpeedTree exports down to an instanceable poly count.
    size_t max_tris = 0;
    // tree rendering (tree rendering): --emit-lods also writes coarser LOD variants
    // ("<stem>.lod1.lmesh", "<stem>.lod2.lmesh") next to the LOD0 output by
    // re-running the SAME meshopt decimation path at successively smaller triangle
    // budgets. The renderer (Luminumbra::Rendering::LodMeshPath / SelectTreeLod)
    // picks these by camera distance; missing variants fall back to LOD0. This is
    // a render-only optimization and does not change the LOD0 output.
    bool emit_lods = false;
    for (int a = 3; a < argc; ++a) {
        const std::string arg = argv[a];
        if (arg == "--max-tris" && a + 1 < argc) {
            try {
                max_tris = static_cast<size_t>(std::stoull(argv[++a]));
            } catch (...) {
                std::cerr << "Error: --max-tris needs an integer" << std::endl;
                return 1;
            }
        } else if (arg == "--primitive" && a + 1 < argc) {
            try {
                g_only_primitive = std::stoi(argv[++a]);
            } catch (...) {
                std::cerr << "Error: --primitive needs an integer" << std::endl;
                return 1;
            }
        } else if (arg == "--emit-lods") {
            emit_lods = true;
        } else {
            std::cerr << "Error: unrecognized argument '" << arg << "'" << std::endl;
            return 1;
        }
    }
    g_max_tris = max_tris;
    if (!process_gltf_checked(argv[1], output_path))
        return 1;

    if (emit_lods) {
        // Derive "<stem>.lodN.lmesh" from the LOD0 output path (matches the
        // renderer's LodMeshPath naming). LOD0 = the just-written output. LOD1/LOD2
        // are coarser triangle budgets relative to LOD0's effective budget; if no
        // --max-tris was given we fall back to fixed, modest budgets so distant
        // foliage still drops a large share of its triangles.
        const std::string kExt = ".lmesh";
        std::string stem = output_path;
        if (stem.size() >= kExt.size() &&
            stem.compare(stem.size() - kExt.size(), kExt.size(), kExt) == 0) {
            stem = stem.substr(0, stem.size() - kExt.size());
        }
        const size_t base_budget = (max_tris > 0) ? max_tris : 20000;
        const size_t lod_budgets[2] = {base_budget / 2, base_budget / 6};
        for (int lod = 1; lod <= 2; ++lod) {
            const size_t budget = lod_budgets[lod - 1];
            g_max_tris = (budget > 0) ? budget : 1;
            const std::string lod_path = stem + ".lod" + std::to_string(lod) + kExt;
            std::cout << "Emitting LOD" << lod << " (target " << g_max_tris << " tris) -> '"
                      << lod_path << "'" << std::endl;
            if (!process_gltf_checked(argv[1], lod_path))
                return 1;
        }
    }
    return 0;
}
