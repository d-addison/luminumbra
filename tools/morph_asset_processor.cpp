#include "cgltf.h"
#include "luminumbra_common/animation/MorphMesh.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <utility>

namespace {
using namespace luminumbra::animation;

bool Refuse(const char* message) {
    std::cerr << "morph.profile: " << message << '\n';
    return false;
}

bool Compose(const MorphMatrix& parent, const cgltf_node& node, MorphMatrix& world) {
    if (node.has_matrix && (node.has_translation || node.has_rotation || node.has_scale))
        return false;
    if (!node.has_matrix) {
        double length = 0;
        for (float value : node.rotation)
            length += double(value) * value;
        if (!std::isfinite(length) || std::abs(length - 1) > 1e-4)
            return false;
    }
    MorphMatrix local{};
    cgltf_node_transform_local(&node, local.data());
    if (local[3] != 0 || local[7] != 0 || local[11] != 0 || local[15] != 1)
        return false;
    for (std::size_t col = 0; col < 4; ++col)
        for (std::size_t row = 0; row < 4; ++row) {
            double value = 0;
            for (std::size_t k = 0; k < 4; ++k)
                value += double(parent[k * 4 + row]) * local[col * 4 + k];
            if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
                return false;
            world[col * 4 + row] = static_cast<float>(value);
        }
    return true;
}

bool Select(const cgltf_data& data, const cgltf_node*& selected, MorphMatrix& source_world) {
    const cgltf_scene* scene = data.scene;
    if (!scene && data.scenes_count > 1)
        return Refuse("Select a default scene when the document contains multiple scenes");
    if (!scene && data.scenes_count == 1)
        scene = data.scenes;
    std::vector<std::pair<const cgltf_node*, MorphMatrix>> pending;
    if (scene) {
        if (scene->extensions_count)
            return Refuse("Scene extensions are unavailable");
        for (std::size_t i = scene->nodes_count; i > 0; --i) {
            if (scene->nodes[i - 1]->parent)
                return Refuse("Scene roots must not have an external parent");
            pending.emplace_back(scene->nodes[i - 1], kMorphIdentity);
        }
    } else {
        for (std::size_t i = data.nodes_count; i > 0; --i)
            if (!data.nodes[i - 1].parent)
                pending.emplace_back(&data.nodes[i - 1], kMorphIdentity);
    }
    std::vector<bool> seen(data.nodes_count, false);
    while (!pending.empty()) {
        const auto [node, parent] = pending.back();
        pending.pop_back();
        const auto index = static_cast<std::size_t>(node - data.nodes);
        if (seen[index] || node->extensions_count || node->has_mesh_gpu_instancing || node->skin)
            return Refuse("Repeated/cyclic nodes, node extensions and skinning are unavailable");
        seen[index] = true;
        MorphMatrix world{};
        if (!Compose(parent, *node, world))
            return Refuse("Node transforms must be finite, affine and valid matrix or TRS");
        if (node->mesh) {
            if (selected)
                return Refuse("Export exactly one selected static mesh instance");
            selected = node;
            source_world = world;
        }
        for (std::size_t i = node->children_count; i > 0; --i)
            pending.emplace_back(node->children[i - 1], world);
    }
    return selected || Refuse("No selected morph mesh instance");
}

bool Dense(const cgltf_accessor* accessor, cgltf_type type, std::size_t count) {
    return accessor && !accessor->is_sparse && !accessor->normalized &&
           !accessor->extensions_count && accessor->buffer_view && accessor->type == type &&
           accessor->component_type == cgltf_component_type_r_32f && accessor->count == count;
}

bool SafeAccessors(const cgltf_data& data) {
    // Refuse unsupported sparse/extended data before cgltf_validate examines
    // indices. Subtraction/division checks cannot wrap on hostile offsets/counts.
    for (std::size_t i = 0; i < data.buffer_views_count; ++i) {
        const auto& view = data.buffer_views[i];
        if (view.extensions_count || view.has_meshopt_compression ||
            view.buffer != &data.buffers[0] || view.offset > view.buffer->size ||
            view.size > view.buffer->size - view.offset ||
            (view.stride && (view.stride < 4 || view.stride > 252 || view.stride % 4)))
            return Refuse("Invalid, compressed or overflowing morph buffer view");
    }
    for (std::size_t i = 0; i < data.accessors_count; ++i) {
        const auto& accessor = data.accessors[i];
        const auto size = cgltf_calc_size(accessor.type, accessor.component_type);
        const auto component = cgltf_component_size(accessor.component_type);
        const auto* view = accessor.buffer_view;
        if (!view || accessor.is_sparse || accessor.extensions_count || !size || !component ||
            !accessor.count || accessor.count > kMorphMaxIndices || accessor.stride < size ||
            accessor.stride > 256 || accessor.stride % component || accessor.offset % component ||
            view->offset % component || accessor.offset > view->size ||
            size > view->size - accessor.offset ||
            accessor.count - 1 > (view->size - accessor.offset - size) / accessor.stride)
            return Refuse("Invalid, sparse, misaligned or overflowing morph accessor");
    }
    return true;
}

bool Import(const cgltf_data& data, MorphMeshData& out) {
    const cgltf_node* node = nullptr;
    if (!Select(data, node, out.source_world))
        return false;
    const auto& mesh = *node->mesh;
    if (mesh.extensions_count || mesh.primitives_count != 1)
        return Refuse("Export one primitive without mesh extensions");
    const auto& primitive = mesh.primitives[0];
    if (primitive.type != cgltf_primitive_type_triangles || primitive.extensions_count ||
        primitive.has_draco_mesh_compression || primitive.mappings_count ||
        !primitive.targets_count || primitive.targets_count > kMorphMaxTargets)
        return Refuse("Use an uncompressed triangle primitive with 1..64 morph targets");
    const cgltf_accessor *positions = nullptr, *normals = nullptr, *uv = nullptr;
    for (std::size_t i = 0; i < primitive.attributes_count; ++i) {
        const auto& attribute = primitive.attributes[i];
        const cgltf_accessor** destination = nullptr;
        if (attribute.index == 0 && attribute.type == cgltf_attribute_type_position)
            destination = &positions;
        else if (attribute.index == 0 && attribute.type == cgltf_attribute_type_normal)
            destination = &normals;
        else if (attribute.index == 0 && attribute.type == cgltf_attribute_type_texcoord)
            destination = &uv;
        if (!destination || *destination)
            return Refuse(
                "Only unique POSITION, NORMAL and TEXCOORD_0 base attributes are supported");
        *destination = attribute.data;
    }
    const auto count = positions ? positions->count : 0;
    const auto indices = primitive.indices ? primitive.indices->count : count;
    const auto targets = primitive.targets_count;
    if (!count || count > kMorphMaxVertices || indices < 3 || indices > kMorphMaxIndices ||
        indices % 3 ||
        96ull + 4 * targets + 32 * count + 4 * indices + 24 * count * targets > kMorphMaxBytes)
        return Refuse("Morph topology exceeds the bounded geometry profile");
    if (!Dense(positions, cgltf_type_vec3, count) || !Dense(normals, cgltf_type_vec3, count) ||
        (uv && !Dense(uv, cgltf_type_vec2, count)))
        return Refuse("Supply dense float POSITION/NORMAL and optional TEXCOORD_0");
    if (primitive.indices &&
        (primitive.indices->is_sparse || primitive.indices->normalized ||
         primitive.indices->extensions_count || !primitive.indices->buffer_view ||
         primitive.indices->type != cgltf_type_scalar ||
         (primitive.indices->component_type != cgltf_component_type_r_8u &&
          primitive.indices->component_type != cgltf_component_type_r_16u &&
          primitive.indices->component_type != cgltf_component_type_r_32u)))
        return Refuse("Use dense unsigned scalar triangle indices");
    out.vertices.resize(count);
    for (std::size_t v = 0; v < count; ++v) {
        auto& vertex = out.vertices[v];
        if (!cgltf_accessor_read_float(positions, v, vertex.position.data(), 3) ||
            !cgltf_accessor_read_float(normals, v, vertex.normal.data(), 3) ||
            (uv && !cgltf_accessor_read_float(uv, v, vertex.uv.data(), 2)))
            return Refuse("Could not decode base geometry");
    }
    out.indices.resize(indices);
    for (std::size_t i = 0; i < indices; ++i) {
        const auto index = primitive.indices ? cgltf_accessor_read_index(primitive.indices, i) : i;
        if (index >= count)
            return Refuse("Morph index exceeds base geometry");
        out.indices[i] = static_cast<std::uint32_t>(index);
    }
    out.targets.resize(targets);
    for (std::size_t t = 0; t < targets; ++t) {
        out.targets[t].vertices.resize(count);
        bool position_seen = false, normal_seen = false;
        const auto& target = primitive.targets[t];
        if (!target.attributes_count)
            return Refuse("Morph targets must contain a supported delta attribute");
        for (std::size_t a = 0; a < target.attributes_count; ++a) {
            const auto& attribute = target.attributes[a];
            const bool position = attribute.type == cgltf_attribute_type_position;
            const bool normal = attribute.type == cgltf_attribute_type_normal;
            if (attribute.index != 0 || (!position && !normal) || (position && position_seen) ||
                (normal && normal_seen) || !Dense(attribute.data, cgltf_type_vec3, count))
                return Refuse("Only unique dense POSITION/NORMAL target deltas matching the base "
                              "are supported");
            position_seen |= position;
            normal_seen |= normal;
            for (std::size_t v = 0; v < count; ++v) {
                auto& delta = out.targets[t].vertices[v];
                if (!cgltf_accessor_read_float(attribute.data,
                                               v,
                                               position ? delta.position.data()
                                                        : delta.normal.data(),
                                               3))
                    return Refuse("Could not decode morph delta");
            }
        }
    }
    if ((node->weights_count && node->weights_count != targets) ||
        (mesh.weights_count && mesh.weights_count != targets))
        return Refuse("Default weights must match the target count");
    out.default_weights.resize(targets, 0);
    if (node->weights_count)
        std::copy_n(node->weights, targets, out.default_weights.begin());
    else if (mesh.weights_count)
        std::copy_n(mesh.weights, targets, out.default_weights.begin());
    if (primitive.material)
        out.source_material_index = static_cast<std::uint32_t>(primitive.material - data.materials);
    return true;
}
} // namespace

bool process_morph_gltf_checked(const std::string& input_path, const std::string& output_path) {
    using namespace luminumbra::animation;
    std::ifstream input(input_path, std::ios::binary | std::ios::ate);
    const auto size = input.tellg();
    if (!input || size < 20 || static_cast<std::uint64_t>(size) > kMorphMaxBytes)
        return Refuse("Supply a self-contained GLB of at most 256MiB");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input || input.peek() != std::char_traits<char>::eof())
        return Refuse("GLB changed or could not be read completely");
    const std::uint32_t declared_size = std::uint32_t(bytes[8]) | (std::uint32_t(bytes[9]) << 8) |
                                        (std::uint32_t(bytes[10]) << 16) |
                                        (std::uint32_t(bytes[11]) << 24);
    if (declared_size != bytes.size())
        return Refuse("GLB declared size must equal the complete source file");
    cgltf_options options{};
    cgltf_data* raw = nullptr;
    if (cgltf_parse(&options, bytes.data(), bytes.size(), &raw) != cgltf_result_success)
        return Refuse("Could not parse morph GLB");
    const std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw, cgltf_free);
    if (data->file_type != cgltf_file_type_glb || data->nodes_count > 4096 || data->skins_count ||
        data->animations_count || data->extensions_used_count || data->extensions_required_count ||
        data->data_extensions_count || data->asset.extensions_count || data->buffers_count != 1 ||
        data->buffers[0].uri || data->buffers[0].size > data->bin_size ||
        data->buffers[0].size > kMorphMaxBytes || data->buffers[0].extensions_count)
        return Refuse(
            "Static morph GLB requires one embedded buffer and no skins, animations or extensions");
    for (std::size_t i = 0; i < data->images_count; ++i)
        if (data->images[i].uri)
            return Refuse("External morph source images are unavailable");
    if (!SafeAccessors(*data))
        return false;
    if (cgltf_load_buffers(&options, data.get(), nullptr) != cgltf_result_success ||
        cgltf_validate(data.get()) != cgltf_result_success)
        return Refuse("Morph GLB buffer/accessor validation failed");
    MorphMeshData builder;
    if (!Import(*data, builder))
        return false;
    std::shared_ptr<const MorphMeshAsset> asset;
    std::string error;
    if (!CreateMorphMeshAsset(std::move(builder), asset, &error)) {
        std::cerr << "morph.asset: " << error << '\n';
        return false;
    }
    MorphFrame defaults;
    if (!EvaluateMorphMesh(
            asset, asset->Data().default_weights, kMorphIdentity, defaults, &error)) {
        std::cerr << "morph.defaults: " << error << '\n';
        return false;
    }
    std::vector<std::uint8_t> encoded;
    if (!EncodeMorphMeshAsset(*asset, encoded))
        return Refuse("Morph encoding failed");
    // Validate the complete candidate before opening/replacing any output. This
    // asset write is not a world-save transaction or a power-loss durability claim.
    std::ofstream output(output_path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
    output.close();
    if (!output)
        return Refuse("Could not finish morph output");
    std::cout << "Compiled static morph asset (LMORv1): " << asset->Data().vertices.size()
              << " vertices, " << asset->Data().targets.size() << " targets\n";
    return true;
}
