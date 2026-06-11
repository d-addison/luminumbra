#include <iostream>
#include <vector>
#include <fstream>
#include <string>
#include <cmath>
#include <cfloat>
#include <algorithm>
#include <unordered_map>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#include "meshoptimizer.h" // This will now be found via the include path in CMake

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
        out[0] = 255; out[1] = 0; out[2] = 0; out[3] = 0;
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
            if (remainders[i] > remainders[best]) best = i;
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
    if (node->name && node->name[0] != '\0') return node->name;
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
            if (!channel->target_node || !channel->sampler) continue;

            AnimTargetType targetType;
            uint32_t componentCount;
            switch (channel->target_path) {
                case cgltf_animation_path_type_translation:
                    targetType = AnimTargetType::Translation; componentCount = 3; break;
                case cgltf_animation_path_type_rotation:
                    targetType = AnimTargetType::Rotation; componentCount = 4; break;
                case cgltf_animation_path_type_scale:
                    targetType = AnimTargetType::Scale; componentCount = 3; break;
                default:
                    continue; // morph weights unsupported in v1 .lanim
            }

            const cgltf_accessor* input = channel->sampler->input;
            const cgltf_accessor* output = channel->sampler->output;
            if (!input || !output || input->count == 0 || output->count < input->count) continue;

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

        if (trackHeaders.empty()) continue;
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
            outFile.write(reinterpret_cast<const char*>(&trackHeaders[t]), sizeof(LanimTrackHeader));
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
// meshoptimizer remap and writes the skeleton block plus sibling .lanim clips.
void process_skinned_gltf(cgltf_data* data, const std::string& input_path, const std::string& output_path) {
    const cgltf_skin* skin = &data->skins[0];
    if (skin->joints_count == 0 || skin->joints_count > kMaxJointsPerSkeleton) {
        std::cerr << "Error: Skin in " << input_path << " has " << skin->joints_count
                  << " joints (supported: 1.." << kMaxJointsPerSkeleton << ")." << std::endl;
        return;
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
            if (found != jointIndexByNode.end()) joint.parentIndex = found->second;
        }
        if (skin->inverse_bind_matrices) {
            cgltf_accessor_read_float(skin->inverse_bind_matrices, j, joint.inverseBind, 16);
        }
        if (node->has_translation) {
            for (int c = 0; c < 3; ++c) joint.localTranslation[c] = node->translation[c];
        }
        if (node->has_rotation) {
            for (int c = 0; c < 4; ++c) joint.localRotation[c] = node->rotation[c];
        }
        if (node->has_scale) {
            for (int c = 0; c < 3; ++c) joint.localScale[c] = node->scale[c];
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
                if (attr->type == cgltf_attribute_type_position) pos_accessor = attr->data;
                if (attr->type == cgltf_attribute_type_normal) norm_accessor = attr->data;
                if (attr->type == cgltf_attribute_type_texcoord) uv_accessor = attr->data;
                if (attr->type == cgltf_attribute_type_joints && attr->index == 0) joints_accessor = attr->data;
                if (attr->type == cgltf_attribute_type_weights && attr->index == 0) weights_accessor = attr->data;
            }

            if (!index_accessor || !pos_accessor || !norm_accessor || !uv_accessor ||
                !joints_accessor || !weights_accessor) {
                std::cerr << "Warning: Skipping skinned primitive " << prim_idx << " in mesh " << mesh_idx
                          << " due to missing attributes." << std::endl;
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
                        return;
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
        return;
    }

    // meshoptimizer pipeline with the skinned stride so joints/weights survive
    // the remap untouched.
    std::vector<unsigned int> remap(master_raw_vertices.size());
    size_t unique_vertex_count = meshopt_generateVertexRemap(
        remap.data(), master_indices.data(), master_indices.size(),
        master_raw_vertices.data(), master_raw_vertices.size(), sizeof(SkinnedVertexData));

    std::vector<uint32_t> optimized_indices(master_indices.size());
    meshopt_remapIndexBuffer(optimized_indices.data(), master_indices.data(), master_indices.size(), remap.data());

    std::vector<SkinnedVertexData> optimized_vertices(unique_vertex_count);
    meshopt_remapVertexBuffer(optimized_vertices.data(), master_raw_vertices.data(),
                              master_raw_vertices.size(), sizeof(SkinnedVertexData), remap.data());

    meshopt_optimizeVertexCache(optimized_indices.data(), optimized_indices.data(),
                                optimized_indices.size(), unique_vertex_count);
    meshopt_optimizeOverdraw(optimized_indices.data(), optimized_indices.data(), optimized_indices.size(),
                             &optimized_vertices[0].pos[0], unique_vertex_count, sizeof(SkinnedVertexData), 1.05f);
    meshopt_optimizeVertexFetch(optimized_vertices.data(), optimized_indices.data(), optimized_indices.size(),
                                optimized_vertices.data(), unique_vertex_count, sizeof(SkinnedVertexData));

    float min_ext[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float max_ext[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
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
        return;
    }

    outFile.write(reinterpret_cast<const char*>(&header), sizeof(header));
    outFile.write(reinterpret_cast<const char*>(optimized_vertices.data()),
                  static_cast<std::streamsize>(unique_vertex_count * sizeof(SkinnedVertexData)));
    outFile.write(reinterpret_cast<const char*>(optimized_indices.data()),
                  static_cast<std::streamsize>(optimized_indices.size() * sizeof(uint32_t)));
    outFile.write(reinterpret_cast<const char*>(joints.data()),
                  static_cast<std::streamsize>(joints.size() * sizeof(Lms2Joint)));
    outFile.close();

    std::cout << "Successfully processed skinned '" << input_path << "' -> '" << output_path << "' (LMS2)" << std::endl;
    std::cout << "  - Vertices: " << master_raw_vertices.size() << " -> " << unique_vertex_count << std::endl;
    std::cout << "  - Indices: " << optimized_indices.size() << std::endl;
    std::cout << "  - Joints: " << joints.size() << std::endl;

    WriteAnimationClips(data, output_path);
}

} // namespace

void process_gltf(const std::string& input_path, const std::string& output_path) {
    cgltf_options options = {};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&options, input_path.c_str(), &data) != cgltf_result_success) {
        std::cerr << "Error: Could not parse glTF file: " << input_path << std::endl;
        return;
    }

    if (cgltf_load_buffers(&options, data, input_path.c_str()) != cgltf_result_success) {
        std::cerr << "Error: Could not load glTF buffers for: " << input_path << std::endl;
        cgltf_free(data);
        return;
    }

    // Skinned assets take the LMS2 path; unskinned input continues through the
    // original v1 writer below, byte-identical to previous releases.
    if (data->skins_count > 0) {
        process_skinned_gltf(data, input_path, output_path);
        cgltf_free(data);
        return;
    }

    // FIX: Create master lists to hold combined geometry from all primitives.
    std::vector<Vertex> master_raw_vertices;
    std::vector<uint32_t> master_indices;
    size_t vertex_offset = 0;

    // FIX: Loop through all meshes and all primitives to combine them.
    for (size_t mesh_idx = 0; mesh_idx < data->meshes_count; ++mesh_idx) {
        for (size_t prim_idx = 0; prim_idx < data->meshes[mesh_idx].primitives_count; ++prim_idx) {
            cgltf_primitive* primitive = &data->meshes[mesh_idx].primitives[prim_idx];
            
            cgltf_accessor* index_accessor = primitive->indices;
            cgltf_accessor* pos_accessor = nullptr;
            cgltf_accessor* norm_accessor = nullptr;
            cgltf_accessor* uv_accessor = nullptr;

            for (size_t i = 0; i < primitive->attributes_count; ++i) {
                cgltf_attribute* attr = &primitive->attributes[i];
                if (attr->type == cgltf_attribute_type_position) pos_accessor = attr->data;
                if (attr->type == cgltf_attribute_type_normal) norm_accessor = attr->data;
                if (attr->type == cgltf_attribute_type_texcoord) uv_accessor = attr->data;
            }

            if (!index_accessor || !pos_accessor || !norm_accessor || !uv_accessor) {
                std::cerr << "Warning: Skipping primitive " << prim_idx << " in mesh " << mesh_idx << " due to missing attributes." << std::endl;
                continue;
            }

            // Read indices for this primitive
            for (size_t i = 0; i < index_accessor->count; ++i) {
                // Add the current vertex offset to each index before adding it to the master list
                master_indices.push_back(cgltf_accessor_read_index(index_accessor, i) + (uint32_t)vertex_offset);
            }

            // Read vertices for this primitive
            size_t current_vertex_count = pos_accessor->count;
            for (size_t v = 0; v < current_vertex_count; ++v) {
                Vertex vert;
                cgltf_accessor_read_float(pos_accessor, v, vert.pos, 3);
                cgltf_accessor_read_float(norm_accessor, v, vert.norm, 3);
                cgltf_accessor_read_float(uv_accessor, v, vert.uv, 2);
                master_raw_vertices.push_back(vert);
            }

            // Update the vertex offset for the next primitive
            vertex_offset += current_vertex_count;
        }
    }

    if (master_raw_vertices.empty()) {
        std::cerr << "Error: No valid primitives found in " << input_path << std::endl;
        cgltf_free(data);
        return;
    }

    // Now, run the optimization pipeline on the combined geometry
    std::vector<unsigned int> remap(master_raw_vertices.size());
    size_t unique_vertex_count = meshopt_generateVertexRemap(remap.data(), master_indices.data(), master_indices.size(), master_raw_vertices.data(), master_raw_vertices.size(), sizeof(Vertex));

    std::vector<uint32_t> optimized_indices(master_indices.size());
    meshopt_remapIndexBuffer(optimized_indices.data(), master_indices.data(), master_indices.size(), remap.data());

    std::vector<Vertex> optimized_vertices(unique_vertex_count);
    meshopt_remapVertexBuffer(optimized_vertices.data(), master_raw_vertices.data(), master_raw_vertices.size(), sizeof(Vertex), remap.data());

    meshopt_optimizeVertexCache(optimized_indices.data(), optimized_indices.data(), optimized_indices.size(), unique_vertex_count);
    meshopt_optimizeOverdraw(optimized_indices.data(), optimized_indices.data(), optimized_indices.size(), &optimized_vertices[0].pos[0], unique_vertex_count, sizeof(Vertex), 1.05f);
    meshopt_optimizeVertexFetch(optimized_vertices.data(), optimized_indices.data(), optimized_indices.size(), optimized_vertices.data(), unique_vertex_count, sizeof(Vertex));

    float min_ext[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
    float max_ext[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

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

    std::ofstream outFile(output_path, std::ios::binary);
    if (!outFile) {
        std::cerr << "Error: Could not open output file " << output_path << std::endl;
        cgltf_free(data);
        return;
    }

    LMeshHeader header;
    header.magic = *reinterpret_cast<const uint32_t*>("LMSH");
    header.vertexCount = (uint32_t)unique_vertex_count;
    header.indexCount = (uint32_t)optimized_indices.size();
    memcpy(header.boundingSphere, sphere, sizeof(sphere));

    outFile.write(reinterpret_cast<const char*>(&header), sizeof(LMeshHeader));
    outFile.write(reinterpret_cast<const char*>(optimized_vertices.data()), unique_vertex_count * sizeof(Vertex));
    outFile.write(reinterpret_cast<const char*>(optimized_indices.data()), optimized_indices.size() * sizeof(uint32_t));
    outFile.close();

    std::cout << "Successfully processed '" << input_path << "' -> '" << output_path << "'" << std::endl;
    std::cout << "  - Vertices: " << master_raw_vertices.size() << " -> " << unique_vertex_count << std::endl;
    std::cout << "  - Indices: " << optimized_indices.size() << std::endl;

    cgltf_free(data);
}

int main(int argc, char* argv[]) {
    if (argc != 3) {
        std::cerr << "Usage: AssetProcessor.exe <input.glb> <output.lmesh>" << std::endl;
        return 1;
    }
    process_gltf(argv[1], argv[2]);
    return 0;
}