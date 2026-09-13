#include "PrefabRuntime.h"

#include "PrefabDigest.h"
#include "PrefabValidation.h"
#include "core/FilesystemPath.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <functional>
#include <set>
#include <span>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Luminumbra::Authoring {
namespace {
using Json = nlohmann::json;
using Detail::Require;
constexpr std::size_t kDocumentLimit = 1024 * 1024;
constexpr std::size_t kOutputLimit = 256 * 1024 * 1024;

void Fields(const Json& value, std::initializer_list<std::string_view> names) {
    Require(value.is_object() && value.size() == names.size(), "Unsupported prefab fields");
    for (const auto name : names)
        Require(value.contains(std::string(name)), "Missing prefab field");
}

double Number(const Json& value, double low = -1e30, double high = 1e30) {
    Require(value.is_number(), "Prefab value must be numeric");
    const double result = value.get<double>();
    Require(std::isfinite(result) && result >= low && result <= high,
            "Prefab number is outside the supported range");
    return result;
}

void Vector(const Json& value, std::size_t length, double low = -1e30, double high = 1e30) {
    Require(value.is_array() && value.size() == length, "Invalid prefab vector");
    for (const auto& item : value)
        Number(item, low, high);
}

std::string Id(const Json& value) {
    const auto result = value.get<std::string>();
    Require(Detail::Identifier(result), "Invalid persistent prefab ID");
    return result;
}

bool MemberName(const std::string& name, char kind, std::string_view extension) {
    return name.starts_with(std::string(1, kind) + "-") && name.size() == 34 + extension.size() &&
           name.ends_with(extension) && Detail::Hex(name.substr(2, 32), 32);
}

std::filesystem::path Child(const std::filesystem::path& parent, const std::string& name) {
    Require(!name.empty() && name != "." && name != ".." &&
                name.find_first_of("/\\:\0", 0, 4) == std::string::npos,
            "Invalid prefab member path");
    const auto path = parent / name;
    const auto status = std::filesystem::symlink_status(path);
    bool redirected = std::filesystem::is_symlink(status);
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    redirected = redirected || (attributes != INVALID_FILE_ATTRIBUTES &&
                                (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
#endif
    Require(!redirected, "Prefab path traverses a symlink or reparse point");
    return path;
}

std::vector<std::uint8_t> Read(const std::filesystem::path& path, std::size_t limit) {
    Require(std::filesystem::is_regular_file(path), "Missing regular prefab member");
    const auto size = std::filesystem::file_size(path);
    Require(size > 0 && size <= limit, "Prefab member exceeds its byte budget");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream stream(path, std::ios::binary);
    stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    Require(stream.good() && stream.peek() == std::char_traits<char>::eof(),
            "Prefab member changed or could not be read completely");
    return bytes;
}

Json Document(const std::vector<std::uint8_t>& bytes) {
    Require(bytes.size() <= kDocumentLimit, "Prefab JSON exceeds 1 MiB");
    std::vector<std::set<std::string>> keys;
    return Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json& value) {
        Require(depth <= 64, "Prefab JSON nesting exceeds limit");
        if (event == Json::parse_event_t::object_start)
            keys.emplace_back();
        if (event == Json::parse_event_t::key)
            Require(keys.back().insert(value.get<std::string>()).second,
                    "Duplicate prefab JSON key");
        if (event == Json::parse_event_t::object_end)
            keys.pop_back();
        return true;
    });
}

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> bytes)
        : m_bytes(bytes) {}
    std::uint32_t Unsigned(std::size_t count = 4) {
        Require(m_offset + count <= m_bytes.size(), "Truncated compiled prefab member");
        std::uint32_t value = 0;
        for (std::size_t i = 0; i < count; ++i)
            value |= static_cast<std::uint32_t>(m_bytes[m_offset++]) << (8 * i);
        return value;
    }
    float Float() {
        const auto value = std::bit_cast<float>(Unsigned());
        Require(std::isfinite(value), "Nonfinite compiled mesh attribute");
        return value;
    }

private:
    std::span<const std::uint8_t> m_bytes;
    std::size_t m_offset = 0;
};

std::shared_ptr<const PrefabMesh> Mesh(std::vector<std::uint8_t> bytes, const Json& entry) {
    Fields(entry, {"format", "vertices", "triangles", "joints", "bytes", "sha256"});
    auto mesh = std::make_shared<PrefabMesh>();
    Reader reader(bytes);
    Require(reader.Unsigned() == 0x48534d4c, "Expected static LMSH geometry");
    mesh->vertex_count = reader.Unsigned();
    mesh->index_count = reader.Unsigned();
    for (int i = 0; i < 3; ++i)
        reader.Float();
    Require(reader.Float() >= 0, "Invalid compiled bounding sphere");
    Require(mesh->vertex_count > 0 && mesh->index_count > 0 && mesh->index_count % 3 == 0 &&
                bytes.size() == 28ull + 32ull * mesh->vertex_count + 4ull * mesh->index_count,
            "Invalid compiled mesh extent");
    Require(entry.at("format") == "LMSH" && entry.at("vertices") == mesh->vertex_count &&
                entry.at("triangles") == mesh->index_count / 3 && entry.at("joints") == 0,
            "Compiled mesh metrics disagree with manifest");
    for (std::uint32_t v = 0; v < mesh->vertex_count; ++v) {
        glm::dvec3 position;
        for (int i = 0; i < 3; ++i)
            position[i] = reader.Float();
        for (int i = 0; i < 5; ++i)
            reader.Float();
        mesh->bounds.minimum = v == 0 ? position : glm::min(mesh->bounds.minimum, position);
        mesh->bounds.maximum = v == 0 ? position : glm::max(mesh->bounds.maximum, position);
    }
    for (std::uint32_t i = 0; i < mesh->index_count; ++i)
        Require(reader.Unsigned() < mesh->vertex_count, "Compiled mesh index out of bounds");
    mesh->bytes = std::move(bytes);
    return mesh;
}

std::shared_ptr<const PrefabTexture> Texture(std::vector<std::uint8_t> bytes, const Json& entry) {
    Fields(entry, {"format", "width", "height", "channels", "mip_levels", "bytes", "sha256"});
    auto texture = std::make_shared<PrefabTexture>();
    Reader reader(bytes);
    Require(reader.Unsigned() == 0x5845544c && reader.Unsigned(2) == 1, "Expected LTEX v1");
    texture->mip_levels = static_cast<std::uint16_t>(reader.Unsigned(2));
    texture->width = reader.Unsigned();
    texture->height = reader.Unsigned();
    Require(reader.Unsigned(1) == 4 && texture->width >= 1 && texture->width <= 4096 &&
                texture->height >= 1 && texture->height <= 4096,
            "Invalid compiled texture layout");
    std::size_t size = 17;
    auto w = texture->width, h = texture->height;
    unsigned levels = 0;
    for (;;) {
        size += static_cast<std::size_t>(w) * h * 4;
        ++levels;
        if (w == 1 && h == 1)
            break;
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
    }
    Require(bytes.size() == size && texture->mip_levels == levels,
            "Incomplete compiled texture mip chain");
    Require(entry.at("format") == "LTEX" && entry.at("width") == texture->width &&
                entry.at("height") == texture->height && entry.at("channels") == 4 &&
                entry.at("mip_levels") == texture->mip_levels,
            "Compiled texture metrics disagree with manifest");
    texture->bytes = std::move(bytes);
    return texture;
}

void Enumerator(const Json& value, std::initializer_list<int> supported) {
    Require(value.is_number_integer(), "Sampler enumerant must be an integer");
    const auto number = value.get<std::int64_t>();
    Require(std::find(supported.begin(), supported.end(), number) != supported.end(),
            "Unsupported sampler enumerant");
}

std::shared_ptr<const PrefabMaterial>
Material(const Json& value,
         const std::map<std::string, std::shared_ptr<const PrefabTexture>>& textures,
         std::set<std::string>& referenced) {
    Fields(value,
           {"base_color",
            "metallic",
            "roughness",
            "emissive",
            "alpha_mode",
            "alpha_cutoff",
            "double_sided",
            "textures",
            "normal_scale",
            "occlusion_strength"});
    Vector(value.at("base_color"), 4, 0, 1);
    Vector(value.at("emissive"), 3, 0, 1);
    for (const auto key : {"metallic", "roughness", "alpha_cutoff", "occlusion_strength"})
        Number(value.at(key), 0, 1);
    Number(value.at("normal_scale"), 0);
    Require(value.at("double_sided").is_boolean(), "Invalid double-sided material flag");
    const auto mode = value.at("alpha_mode").get<std::string>();
    Require(mode == "OPAQUE" || mode == "MASK" || mode == "BLEND", "Unsupported alpha mode");
    const auto& maps = value.at("textures");
    Require(maps.is_object() && maps.size() <= 5, "Invalid material texture map");
    auto material = std::make_shared<PrefabMaterial>();
    material->properties = value;
    Json common_coordinates;
    for (const auto& [role, map] : maps.items()) {
        const bool srgb = role == "baseColorTexture" || role == "emissiveTexture";
        Require(srgb || role == "metallicRoughnessTexture" || role == "normalTexture" ||
                    role == "occlusionTexture",
                "Unsupported material texture role");
        Fields(map, {"file", "encoding", "sampler", "coordinates"});
        Require(map.at("encoding") == (srgb ? "srgb" : "linear"), "Invalid texture encoding");
        const auto file = map.at("file").get<std::string>();
        Require(textures.contains(file), "Missing material texture reference");
        referenced.insert(file);
        material->textures.emplace(role, textures.at(file));
        const auto& sampler = map.at("sampler");
        Fields(sampler, {"wrapS", "wrapT", "magFilter", "minFilter"});
        Enumerator(sampler.at("wrapS"), {10497, 33071, 33648});
        Enumerator(sampler.at("wrapT"), {10497, 33071, 33648});
        Enumerator(sampler.at("magFilter"), {9728, 9729});
        Enumerator(sampler.at("minFilter"), {9728, 9729, 9984, 9985, 9986, 9987});
        const auto& coordinates = map.at("coordinates");
        Fields(coordinates, {"set", "offset", "scale", "rotation"});
        Enumerator(coordinates.at("set"), {0, 1, 2, 3, 4, 5, 6, 7});
        Vector(coordinates.at("offset"), 2);
        Vector(coordinates.at("scale"), 2);
        Number(coordinates.at("rotation"));
        if (common_coordinates.is_null())
            common_coordinates = coordinates;
        Require(common_coordinates == coordinates, "Material maps require a shared UV transform");
    }
    return material;
}
} // namespace

std::shared_ptr<const PrefabAsset> PrefabAsset::Load(const std::filesystem::path& project_root,
                                                     const std::string& generation_id,
                                                     const std::string& manifest_sha256) {
    Require(Detail::Hex(generation_id, 32),
            "Expected a pinned generation ID (32 lowercase hex digits)");
    Require(Detail::Hex(manifest_sha256, 64), "Expected a pinned manifest SHA-256");
    // The caller selects the trusted project root. Descendants cannot redirect us
    // through links, junctions, absolute paths or parent-directory components.
    const auto root = Filesystem::AbsolutePath(project_root);
    const auto directory =
        Child(Child(Child(root, ".luminumbra-author"), "generations"), generation_id);
    const auto manifest_bytes = Read(Child(directory, "manifest.json"), kDocumentLimit);
    Require(Detail::Sha256(manifest_bytes) == manifest_sha256, "Prefab manifest hash mismatch");
    const auto manifest = Document(manifest_bytes);
    Fields(manifest,
           {"schema",
            "mode",
            "job_id",
            "asset_id",
            "revision",
            "build_identity",
            "input_hashes",
            "outputs"});
    Require(manifest.at("schema") == "luminumbra.authoring.generation.v1" &&
                manifest.at("mode") == "NATIVE" && manifest.at("job_id") == generation_id,
            "Unsupported prefab generation identity");
    auto asset = std::shared_ptr<PrefabAsset>(new PrefabAsset());
    asset->m_asset_id = Id(manifest.at("asset_id"));
    asset->m_generation_id = generation_id;
    asset->m_manifest_sha256 = manifest_sha256;
    const auto& outputs = manifest.at("outputs");
    Require(outputs.is_object() && outputs.contains("prefab.json") && outputs.size() >= 2 &&
                outputs.size() <= 129,
            "Invalid prefab generation member count");
    std::map<std::string, std::shared_ptr<const PrefabMesh>> meshes;
    std::map<std::string, std::shared_ptr<const PrefabTexture>> textures;
    Json descriptor;
    std::size_t total = 0;
    for (const auto& [name, entry] : outputs.items()) {
        Require(name == "prefab.json" || MemberName(name, 'm', ".lmesh") ||
                    MemberName(name, 't', ".ltex"),
                "Unsupported prefab generation member name");
        auto bytes = Read(Child(directory, name), kOutputLimit - total);
        total += bytes.size();
        const auto hash = entry.at("sha256").get<std::string>();
        Require(Detail::Hex(hash, 64) && Detail::Sha256(bytes) == hash &&
                    entry.at("bytes") == bytes.size(),
                "Prefab member hash or byte count mismatch");
        if (name == "prefab.json") {
            Fields(entry, {"format", "schema", "nodes", "meshes", "materials", "bytes", "sha256"});
            Require(entry.at("format") == "PREFAB" &&
                        entry.at("schema") == "luminumbra.asset.prefab.v1",
                    "Unsupported prefab descriptor format");
            descriptor = Document(bytes);
        } else if (MemberName(name, 'm', ".lmesh")) {
            meshes.emplace(name, Mesh(std::move(bytes), entry));
        } else {
            textures.emplace(name, Texture(std::move(bytes), entry));
        }
    }
    for (const auto& member : std::filesystem::directory_iterator(directory)) {
        const auto name = member.path().filename().string();
        Require(name == "manifest.json" || outputs.contains(name),
                "Unlisted prefab generation member");
    }
    Fields(descriptor,
           {"schema",
            "asset_id",
            "coordinates",
            "nodes",
            "meshes",
            "materials",
            "runtime_components"});
    Require(descriptor.at("schema") == "luminumbra.asset.prefab.v1" &&
                descriptor.at("asset_id") == asset->m_asset_id &&
                descriptor.at("coordinates") == "gltf-rh-y-up-meters" &&
                descriptor.at("runtime_components") == false,
            "Unsupported prefab descriptor contract");
    std::set<std::string> referenced;
    std::map<std::string, std::shared_ptr<const PrefabMaterial>> materials;
    const auto& material_descriptions = descriptor.at("materials");
    Require(material_descriptions.is_object() && !material_descriptions.empty() &&
                material_descriptions.size() <= 128,
            "Invalid prefab materials");
    for (const auto& [id, material] : material_descriptions.items()) {
        Require(Detail::Identifier(id), "Invalid persistent material ID");
        materials.emplace(id, Material(material, textures, referenced));
    }
    std::map<std::string, std::vector<PrefabDraw>> mesh_draws;
    const auto& mesh_descriptions = descriptor.at("meshes");
    Require(mesh_descriptions.is_object() && !mesh_descriptions.empty() &&
                mesh_descriptions.size() <= 4096,
            "Invalid prefab meshes");
    std::set<std::string> used_materials;
    for (const auto& [id, draws] : mesh_descriptions.items()) {
        Require(Detail::Identifier(id) && draws.is_array() && !draws.empty() && draws.size() <= 64,
                "Invalid prefab draw list");
        for (const auto& draw : draws) {
            Fields(draw, {"file", "material", "uv_transform_baked"});
            const auto file = draw.at("file").get<std::string>();
            const auto material = Id(draw.at("material"));
            Require(draw.at("uv_transform_baked") == false && meshes.contains(file) &&
                        materials.contains(material),
                    "Invalid prefab draw reference or UV contract");
            mesh_draws[id].push_back({file, material, meshes.at(file), materials.at(material)});
            referenced.insert(file);
            used_materials.insert(material);
        }
    }
    Require(referenced.size() + 1 == outputs.size() && used_materials.size() == materials.size(),
            "Unreferenced prefab asset or material");
    const auto& descriptions = descriptor.at("nodes");
    Require(descriptions.is_array() && !descriptions.empty() && descriptions.size() <= 4096,
            "Invalid prefab hierarchy extent");
    std::map<std::string, std::size_t> indices;
    std::vector<PrefabNode> nodes;
    std::vector<bool> mirrored;
    std::set<std::string> used_meshes;
    for (const auto& description : descriptions) {
        if (description.contains("mesh"))
            Fields(description,
                   {"id", "label", "parent", "local_matrix", "reverse_front_face", "mesh"});
        else
            Fields(description, {"id", "label", "parent", "local_matrix", "reverse_front_face"});
        PrefabNode node;
        node.id = Id(description.at("id"));
        Require(indices.emplace(node.id, nodes.size()).second,
                "Duplicate persistent prefab node ID");
        node.label = description.at("label").get<std::string>();
        if (!description.at("parent").is_null())
            node.parent_id = Id(description.at("parent"));
        const auto& matrix = description.at("local_matrix");
        Vector(matrix, 16);
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                node.local_matrix[c][r] = Number(matrix[c * 4 + r]);
        Detail::Affine(node.local_matrix);
        for (int a = 0; a < 3; ++a)
            for (int b = a + 1; b < 3; ++b) {
                const glm::dvec3 x(node.local_matrix[a]), y(node.local_matrix[b]);
                Require(std::abs(glm::dot(x, y)) <= glm::length(x) * glm::length(y) * 1e-5,
                        "Local shear is outside the compiled prefab v1 contract");
            }
        if (description.contains("mesh")) {
            const auto mesh = Id(description.at("mesh"));
            Require(mesh_draws.contains(mesh), "Missing prefab mesh reference");
            node.mesh_id = mesh;
            node.draws = mesh_draws.at(mesh);
            used_meshes.insert(mesh);
        }
        Require(description.at("reverse_front_face").is_boolean(), "Invalid prefab winding flag");
        mirrored.push_back(description.at("reverse_front_face").get<bool>());
        nodes.push_back(std::move(node));
    }
    Require(used_meshes.size() == mesh_draws.size(), "Unreferenced prefab mesh definition");
    // Iterative parent walks avoid recursion even for the full 4096-node chain.
    std::vector<unsigned char> state(nodes.size(), 0);
    for (std::size_t start = 0; start < nodes.size(); ++start) {
        if (state[start] == 2)
            continue;
        std::vector<std::size_t> chain;
        auto index = start;
        while (state[index] != 2) {
            Require(state[index] != 1, "Cycle in prefab hierarchy");
            state[index] = 1;
            chain.push_back(index);
            const auto& parent = nodes[index].parent_id;
            if (parent.empty())
                break;
            Require(indices.contains(parent), "Missing prefab parent reference");
            index = indices.at(parent);
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
            auto& node = nodes[*it];
            node.world_matrix =
                node.parent_id.empty()
                    ? node.local_matrix
                    : nodes[indices.at(node.parent_id)].world_matrix * node.local_matrix;
            Detail::Affine(node.world_matrix);
            Require((glm::determinant(glm::dmat3(node.world_matrix)) < 0) == mirrored[*it],
                    "Prefab winding flag disagrees with hierarchy transform");
            state[*it] = 2;
            asset->m_nodes.push_back(node);
        }
    }
    const auto& metrics = outputs.at("prefab.json");
    Require(metrics.at("nodes") == nodes.size() && metrics.at("meshes") == mesh_draws.size() &&
                metrics.at("materials") == materials.size(),
            "Prefab manifest metrics disagree with descriptor");
    return asset;
}
} // namespace Luminumbra::Authoring
