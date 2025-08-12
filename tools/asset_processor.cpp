#include <iostream>
#include <vector>
#include <fstream>
#include <string>
#include <cmath>
#include <cfloat>

#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#include "meshoptimizer.h" // This will now be found via the include path in CMake

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