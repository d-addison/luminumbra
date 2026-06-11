#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "nlohmann/json.hpp"
#include "core/JobSystem.h"
#include "systems/SHIELD_WorldSystem.h"
#include "systems/PhysicsSystem.h"
#include "systems/WaterSystem.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"

namespace fs = std::filesystem;

using namespace Luminumbra;
using namespace Luminumbra::Systems;

namespace {

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

constexpr int kSeed = 424242;
constexpr IVec3 kChunkCoords{0, 0, 0};
// The cave surface cap (kCaveSurfaceCapDepth/kCaveSurfaceFullDepth) suppresses
// carving until ~24 units below the surface, so cave-layer deltas are
// snapshotted in a chunk deep enough for carving to be fully active.
constexpr IVec3 kCaveChunkCoords{0, -3, 0};
constexpr float kDeltaEpsilon = 1.0e-5f;

struct ScalarMetrics {
    std::size_t count = 0;
    float min = 0.0f;
    float max = 0.0f;
    double mean = 0.0;
};

struct SdfMetrics {
    ScalarMetrics values;
    std::size_t solid_samples = 0;
    std::size_t air_samples = 0;
    std::size_t near_surface_samples = 0;
    std::size_t zero_crossing_edges = 0;
};

struct MeshMetrics {
    std::size_t vertices = 0;
    std::size_t indices = 0;
    std::size_t triangles = 0;
    std::size_t invalid_indices = 0;
    std::size_t degenerate_triangles = 0;
    std::size_t bad_vertex_normals = 0;
};

struct SampleLayerMetrics {
    ScalarMetrics base_height;
    ScalarMetrics final_height;
    ScalarMetrics island_mask;
    ScalarMetrics cave_density;
    ScalarMetrics final_density;
    std::array<std::size_t, 8> material_counts{};
    float max_sdf_sample_error = 0.0f;
    double mean_sdf_sample_error = 0.0;
};

struct LayerSnapshot {
    std::string name;
    int mesh_step = 1;
    std::vector<float> sdf_data;
    std::vector<float> heightmap_data;
    SdfMetrics sdf;
    ScalarMetrics heightmap;
    SampleLayerMetrics sampled_layers;
    MeshMetrics mesh;
    MeshMetrics water_mesh;
};

struct LayerDelta {
    std::string from;
    std::string to;
    std::size_t changed_sdf_samples = 0;
    std::size_t sdf_sign_flips = 0;
    double mean_abs_sdf_delta = 0.0;
    float max_abs_sdf_delta = 0.0f;
    std::size_t changed_height_samples = 0;
    double mean_abs_height_delta = 0.0;
    float max_abs_height_delta = 0.0f;
};

struct AtlasRow {
    std::string preset;
    IVec3 chunk_coords{0};
    float terrain_height = 0.0f;
    float spawn_y = 0.0f;
    LayerSnapshot snapshot;
};

fs::path SourceRoot() {
    return fs::weakly_canonical(fs::path(LUMINUMBRA_SOURCE_ROOT));
}

fs::path ArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "worldgen_layers";
}

TerrainGenParams TerrainLayerParams() {
    TerrainGenParams params;
    params.base_frequency = 0.035f;
    params.base_amplitude = 8.0f;
    params.octaves = 5;
    params.persistence = 0.5f;
    params.lacunarity = 2.1f;
    params.height_offset = 10.0f;
    params.caves_enabled = false;
    params.island_mask_enabled = false;
    params.island_mask_frequency = 0.075f;
    return params;
}

TerrainGenParams CaveLayerParams() {
    TerrainGenParams params = TerrainLayerParams();
    params.caves_enabled = true;
    params.cave_frequency = 0.15f;
    params.cave_threshold = 0.55f;
    params.cave_carve_value = 5.0f;
    return params;
}

TerrainGenParams WaterLayerParams() {
    TerrainGenParams params;
    params.base_frequency = 0.02f;
    params.base_amplitude = 1.0f;
    params.height_offset = -4.0f;
    params.caves_enabled = false;
    params.island_mask_enabled = false;
    return params;
}

TerrainGenParams LoadPresetParams(const fs::path& path) {
    std::ifstream input(path);
    EXPECT_TRUE(input) << path.string();
    nlohmann::json data = nlohmann::json::parse(input);

    const nlohmann::json& gen_params = data.at("generation_params");
    const nlohmann::json& terrain = gen_params.at("terrain");
    const nlohmann::json& features = gen_params.value("features", nlohmann::json::object());

    TerrainGenParams params;
    params.base_frequency = terrain.value("base_frequency", params.base_frequency);
    params.base_amplitude = terrain.value("base_amplitude", params.base_amplitude);
    params.octaves = terrain.value("octaves", params.octaves);
    params.persistence = terrain.value("persistence", params.persistence);
    params.lacunarity = terrain.value("lacunarity", params.lacunarity);
    params.height_offset = terrain.value("height_offset", params.height_offset);
    params.island_mask_enabled = terrain.value("island_mask_enabled", params.island_mask_enabled);
    params.island_mask_frequency = terrain.value("island_mask_frequency", params.island_mask_frequency);
    params.caves_enabled = features.value("caves_enabled", params.caves_enabled);
    params.cave_frequency = features.value("cave_frequency", params.cave_frequency);
    params.cave_threshold = features.value("cave_threshold", params.cave_threshold);
    params.cave_carve_value = features.value("cave_carve_value", params.cave_carve_value);
    return params;
}

ScalarMetrics CalculateScalarMetrics(const std::vector<float>& values) {
    ScalarMetrics metrics;
    metrics.count = values.size();
    if (values.empty()) {
        return metrics;
    }

    metrics.min = std::numeric_limits<float>::max();
    metrics.max = std::numeric_limits<float>::lowest();
    double sum = 0.0;
    for (const float value : values) {
        metrics.min = std::min(metrics.min, value);
        metrics.max = std::max(metrics.max, value);
        sum += static_cast<double>(value);
    }
    metrics.mean = sum / static_cast<double>(values.size());
    return metrics;
}

std::size_t SdfIndex(int x, int y, int z) {
    constexpr int size_x = CHUNK_SIZE_X + 1;
    constexpr int size_y = CHUNK_SIZE_Y + 1;
    return static_cast<std::size_t>(x)
        + static_cast<std::size_t>(y) * size_x
        + static_cast<std::size_t>(z) * size_x * size_y;
}

bool CrossesSurface(float a, float b) {
    return (a < 0.0f && b >= 0.0f) || (a >= 0.0f && b < 0.0f);
}

SdfMetrics CalculateSdfMetrics(const std::vector<float>& values) {
    SdfMetrics metrics;
    metrics.values = CalculateScalarMetrics(values);

    for (const float value : values) {
        if (value < 0.0f) {
            ++metrics.solid_samples;
        } else {
            ++metrics.air_samples;
        }
        if (std::abs(value) <= 0.5f) {
            ++metrics.near_surface_samples;
        }
    }

    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        for (int y = 0; y <= CHUNK_SIZE_Y; ++y) {
            for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
                const float current = values[SdfIndex(x, y, z)];
                if (x < CHUNK_SIZE_X && CrossesSurface(current, values[SdfIndex(x + 1, y, z)])) {
                    ++metrics.zero_crossing_edges;
                }
                if (y < CHUNK_SIZE_Y && CrossesSurface(current, values[SdfIndex(x, y + 1, z)])) {
                    ++metrics.zero_crossing_edges;
                }
                if (z < CHUNK_SIZE_Z && CrossesSurface(current, values[SdfIndex(x, y, z + 1)])) {
                    ++metrics.zero_crossing_edges;
                }
            }
        }
    }

    return metrics;
}

MeshMetrics CalculateMeshMetrics(const std::vector<VoxelVertex>& vertices, const std::vector<u32>& indices) {
    MeshMetrics metrics;
    metrics.vertices = vertices.size();
    metrics.indices = indices.size();
    metrics.triangles = indices.size() / 3u;

    for (const VoxelVertex& vertex : vertices) {
        const float normal_length = glm::length(vertex.normal);
        if (!std::isfinite(normal_length) || std::abs(normal_length - 1.0f) > 0.01f) {
            ++metrics.bad_vertex_normals;
        }
    }

    for (std::size_t i = 0; i + 2u < indices.size(); i += 3u) {
        const u32 i0 = indices[i];
        const u32 i1 = indices[i + 1u];
        const u32 i2 = indices[i + 2u];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) {
            ++metrics.invalid_indices;
            continue;
        }

        const Vec3 edge_a = vertices[i1].position - vertices[i0].position;
        const Vec3 edge_b = vertices[i2].position - vertices[i0].position;
        if (glm::length(glm::cross(edge_a, edge_b)) < 1.0e-6f) {
            ++metrics.degenerate_triangles;
        }
    }

    return metrics;
}

std::size_t MaterialIndex(MaterialType material) {
    const std::size_t index = static_cast<std::size_t>(material);
    return index < 8u ? index : 0u;
}

SampleLayerMetrics CalculateSampleLayerMetrics(const SHIELD_WorldSystem& world, const Chunk& chunk) {
    const IVec3 base_pos = chunk.get_coords() * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
    SampleLayerMetrics metrics;
    std::vector<float> base_heights;
    std::vector<float> final_heights;
    std::vector<float> island_masks;
    std::vector<float> cave_densities;
    std::vector<float> final_densities;
    base_heights.reserve(static_cast<std::size_t>(CHUNK_SIZE_X + 1) * static_cast<std::size_t>(CHUNK_SIZE_Z + 1));
    final_heights.reserve(base_heights.capacity());
    island_masks.reserve(base_heights.capacity());
    cave_densities.reserve(chunk.sdf_data.size());
    final_densities.reserve(chunk.sdf_data.size());

    double sdf_error_sum = 0.0;
    for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
        for (int y = 0; y <= CHUNK_SIZE_Y; ++y) {
            for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
                const Vec3 world_pos = Vec3(base_pos + IVec3(x, y, z));
                const WorldGenLayerSample sample = world.SampleWorldGenLayers(world_pos);
                const std::size_t sdf_index = SdfIndex(x, y, z);
                const float sdf_error = std::abs(sample.final_density - chunk.sdf_data[sdf_index]);

                metrics.max_sdf_sample_error = std::max(metrics.max_sdf_sample_error, sdf_error);
                sdf_error_sum += static_cast<double>(sdf_error);
                cave_densities.push_back(sample.cave_density);
                final_densities.push_back(sample.final_density);
                ++metrics.material_counts[MaterialIndex(sample.material)];

                if (y == 0) {
                    base_heights.push_back(sample.base_height);
                    final_heights.push_back(sample.final_height);
                    island_masks.push_back(sample.island_mask);
                }
            }
        }
    }

    if (!chunk.sdf_data.empty()) {
        metrics.mean_sdf_sample_error = sdf_error_sum / static_cast<double>(chunk.sdf_data.size());
    }
    metrics.base_height = CalculateScalarMetrics(base_heights);
    metrics.final_height = CalculateScalarMetrics(final_heights);
    metrics.island_mask = CalculateScalarMetrics(island_masks);
    metrics.cave_density = CalculateScalarMetrics(cave_densities);
    metrics.final_density = CalculateScalarMetrics(final_densities);
    return metrics;
}

LayerSnapshot GenerateSnapshot(
    const std::string& name,
    const TerrainGenParams& params,
    int mesh_step,
    bool generate_water,
    const IVec3& chunk_coords = kChunkCoords
) {
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    LayerSnapshot snapshot;
    snapshot.name = name;
    snapshot.mesh_step = mesh_step;
    Chunk chunk(chunk_coords);

    world.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, mesh_step);

    if (generate_water) {
        WaterSystem water(nullptr, &world);
        chunk.water_level_data.assign(
            static_cast<std::size_t>(WATER_SIM_RESOLUTION_X) * static_cast<std::size_t>(WATER_SIM_RESOLUTION_Z),
            SEA_LEVEL);
        chunk.has_water_sim.store(true);
        World::MarchingCubes::GenerateWaterMesh(water, world, chunk);
    }

    snapshot.sdf = CalculateSdfMetrics(chunk.sdf_data);
    snapshot.heightmap = CalculateScalarMetrics(chunk.heightmap_data);
    snapshot.sampled_layers = CalculateSampleLayerMetrics(world, chunk);
    snapshot.mesh = CalculateMeshMetrics(chunk.mesh_vertices, chunk.mesh_indices);
    snapshot.water_mesh = CalculateMeshMetrics(chunk.water_mesh_vertices, chunk.water_mesh_indices);
    snapshot.sdf_data = std::move(chunk.sdf_data);
    snapshot.heightmap_data = std::move(chunk.heightmap_data);
    return snapshot;
}

LayerDelta CalculateDelta(const LayerSnapshot& from, const LayerSnapshot& to) {
    LayerDelta delta;
    delta.from = from.name;
    delta.to = to.name;

    const std::size_t sdf_count = std::min(from.sdf_data.size(), to.sdf_data.size());
    double sdf_sum = 0.0;
    for (std::size_t i = 0; i < sdf_count; ++i) {
        const float abs_delta = std::abs(to.sdf_data[i] - from.sdf_data[i]);
        if (abs_delta > kDeltaEpsilon) {
            ++delta.changed_sdf_samples;
        }
        if (CrossesSurface(from.sdf_data[i], to.sdf_data[i])) {
            ++delta.sdf_sign_flips;
        }
        delta.max_abs_sdf_delta = std::max(delta.max_abs_sdf_delta, abs_delta);
        sdf_sum += static_cast<double>(abs_delta);
    }
    if (sdf_count > 0u) {
        delta.mean_abs_sdf_delta = sdf_sum / static_cast<double>(sdf_count);
    }

    const std::size_t height_count = std::min(from.heightmap_data.size(), to.heightmap_data.size());
    double height_sum = 0.0;
    for (std::size_t i = 0; i < height_count; ++i) {
        const float abs_delta = std::abs(to.heightmap_data[i] - from.heightmap_data[i]);
        if (abs_delta > kDeltaEpsilon) {
            ++delta.changed_height_samples;
        }
        delta.max_abs_height_delta = std::max(delta.max_abs_height_delta, abs_delta);
        height_sum += static_cast<double>(abs_delta);
    }
    if (height_count > 0u) {
        delta.mean_abs_height_delta = height_sum / static_cast<double>(height_count);
    }

    return delta;
}

unsigned char ToByte(float value) {
    const float clamped = std::clamp(value, 0.0f, 255.0f);
    return static_cast<unsigned char>(std::lround(clamped));
}

void WritePpm(const fs::path& path, int width, int height, const std::vector<unsigned char>& pixels) {
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output) << path.string();
    output << "P6\n" << width << " " << height << "\n255\n";
    output.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
}

void WriteHeightmapPpm(const LayerSnapshot& snapshot, const fs::path& path) {
    constexpr int width = CHUNK_SIZE_X + 1;
    constexpr int height = CHUNK_SIZE_Z + 1;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);
    const float range = std::max(0.001f, snapshot.heightmap.max - snapshot.heightmap.min);

    for (int z = 0; z < height; ++z) {
        for (int x = 0; x < width; ++x) {
            const std::size_t sample_index = static_cast<std::size_t>(x) + static_cast<std::size_t>(z) * width;
            const float t = (snapshot.heightmap_data[sample_index] - snapshot.heightmap.min) / range;
            const std::size_t pixel_index = sample_index * 3u;
            pixels[pixel_index] = ToByte(30.0f + 190.0f * t);
            pixels[pixel_index + 1u] = ToByte(55.0f + 160.0f * t);
            pixels[pixel_index + 2u] = ToByte(95.0f + 80.0f * (1.0f - t));
        }
    }

    WritePpm(path, width, height, pixels);
}

void WriteSdfSlicePpm(const LayerSnapshot& snapshot, int slice_y, const fs::path& path) {
    constexpr int width = CHUNK_SIZE_X + 1;
    constexpr int height = CHUNK_SIZE_Z + 1;
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);

    for (int z = 0; z < height; ++z) {
        for (int x = 0; x < width; ++x) {
            const float density = snapshot.sdf_data[SdfIndex(x, slice_y, z)];
            const std::size_t pixel_index = (static_cast<std::size_t>(x) + static_cast<std::size_t>(z) * width) * 3u;
            if (std::abs(density) <= 0.35f) {
                pixels[pixel_index] = 255u;
                pixels[pixel_index + 1u] = 255u;
                pixels[pixel_index + 2u] = 255u;
            } else if (density < 0.0f) {
                pixels[pixel_index] = ToByte(20.0f);
                pixels[pixel_index + 1u] = ToByte(100.0f + std::min(120.0f, -density * 12.0f));
                pixels[pixel_index + 2u] = ToByte(45.0f);
            } else {
                pixels[pixel_index] = ToByte(25.0f);
                pixels[pixel_index + 1u] = ToByte(65.0f);
                pixels[pixel_index + 2u] = ToByte(110.0f + std::min(120.0f, density * 10.0f));
            }
        }
    }

    WritePpm(path, width, height, pixels);
}

std::string JsonNumber(double value) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(6) << value;
    return stream.str();
}

void WriteMetricsJson(const fs::path& path, const std::vector<LayerSnapshot>& snapshots, const std::vector<LayerDelta>& deltas) {
    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();

    output << "{\n";
    output << "  \"schema\": \"luminumbra.worldgen_layers.v1\",\n";
    output << "  \"seed\": " << kSeed << ",\n";
    output << "  \"chunk\": [" << kChunkCoords.x << ", " << kChunkCoords.y << ", " << kChunkCoords.z << "],\n";
    output << "  \"snapshots\": [\n";
    for (std::size_t i = 0; i < snapshots.size(); ++i) {
        const LayerSnapshot& snapshot = snapshots[i];
        output << "    {\n";
        output << "      \"name\": \"" << snapshot.name << "\",\n";
        output << "      \"mesh_step\": " << snapshot.mesh_step << ",\n";
        output << "      \"sdf\": {";
        output << "\"min\": " << JsonNumber(snapshot.sdf.values.min) << ", ";
        output << "\"max\": " << JsonNumber(snapshot.sdf.values.max) << ", ";
        output << "\"mean\": " << JsonNumber(snapshot.sdf.values.mean) << ", ";
        output << "\"solid_samples\": " << snapshot.sdf.solid_samples << ", ";
        output << "\"air_samples\": " << snapshot.sdf.air_samples << ", ";
        output << "\"near_surface_samples\": " << snapshot.sdf.near_surface_samples << ", ";
        output << "\"zero_crossing_edges\": " << snapshot.sdf.zero_crossing_edges << "},\n";
        output << "      \"heightmap\": {";
        output << "\"min\": " << JsonNumber(snapshot.heightmap.min) << ", ";
        output << "\"max\": " << JsonNumber(snapshot.heightmap.max) << ", ";
        output << "\"mean\": " << JsonNumber(snapshot.heightmap.mean) << "},\n";
        output << "      \"sampled_layers\": {";
        output << "\"base_height_mean\": " << JsonNumber(snapshot.sampled_layers.base_height.mean) << ", ";
        output << "\"final_height_mean\": " << JsonNumber(snapshot.sampled_layers.final_height.mean) << ", ";
        output << "\"island_mask_mean\": " << JsonNumber(snapshot.sampled_layers.island_mask.mean) << ", ";
        output << "\"cave_density_mean\": " << JsonNumber(snapshot.sampled_layers.cave_density.mean) << ", ";
        output << "\"final_density_mean\": " << JsonNumber(snapshot.sampled_layers.final_density.mean) << ", ";
        output << "\"max_sdf_sample_error\": " << JsonNumber(snapshot.sampled_layers.max_sdf_sample_error) << ", ";
        output << "\"mean_sdf_sample_error\": " << JsonNumber(snapshot.sampled_layers.mean_sdf_sample_error) << ", ";
        output << "\"material_counts\": [";
        for (std::size_t material = 0; material < snapshot.sampled_layers.material_counts.size(); ++material) {
            output << snapshot.sampled_layers.material_counts[material];
            if (material + 1u != snapshot.sampled_layers.material_counts.size()) {
                output << ", ";
            }
        }
        output << "]},\n";
        output << "      \"mesh\": {";
        output << "\"vertices\": " << snapshot.mesh.vertices << ", ";
        output << "\"indices\": " << snapshot.mesh.indices << ", ";
        output << "\"triangles\": " << snapshot.mesh.triangles << ", ";
        output << "\"invalid_indices\": " << snapshot.mesh.invalid_indices << ", ";
        output << "\"degenerate_triangles\": " << snapshot.mesh.degenerate_triangles << ", ";
        output << "\"bad_vertex_normals\": " << snapshot.mesh.bad_vertex_normals << "},\n";
        output << "      \"water_mesh\": {";
        output << "\"vertices\": " << snapshot.water_mesh.vertices << ", ";
        output << "\"indices\": " << snapshot.water_mesh.indices << ", ";
        output << "\"triangles\": " << snapshot.water_mesh.triangles << "}\n";
        output << "    }" << (i + 1u == snapshots.size() ? "\n" : ",\n");
    }
    output << "  ],\n";
    output << "  \"deltas\": [\n";
    for (std::size_t i = 0; i < deltas.size(); ++i) {
        const LayerDelta& delta = deltas[i];
        output << "    {";
        output << "\"from\": \"" << delta.from << "\", ";
        output << "\"to\": \"" << delta.to << "\", ";
        output << "\"changed_sdf_samples\": " << delta.changed_sdf_samples << ", ";
        output << "\"sdf_sign_flips\": " << delta.sdf_sign_flips << ", ";
        output << "\"mean_abs_sdf_delta\": " << JsonNumber(delta.mean_abs_sdf_delta) << ", ";
        output << "\"max_abs_sdf_delta\": " << JsonNumber(delta.max_abs_sdf_delta) << ", ";
        output << "\"changed_height_samples\": " << delta.changed_height_samples << ", ";
        output << "\"mean_abs_height_delta\": " << JsonNumber(delta.mean_abs_height_delta) << ", ";
        output << "\"max_abs_height_delta\": " << JsonNumber(delta.max_abs_height_delta) << "}";
        output << (i + 1u == deltas.size() ? "\n" : ",\n");
    }
    output << "  ]\n";
    output << "}\n";
}

void WriteSnapshotImages(const fs::path& root, const LayerSnapshot& snapshot) {
    WriteHeightmapPpm(snapshot, root / (snapshot.name + "_height.ppm"));
    for (const int slice_y : std::array<int, 3>{4, 8, 12}) {
        WriteSdfSlicePpm(snapshot, slice_y, root / (snapshot.name + "_sdf_y" + std::to_string(slice_y) + ".ppm"));
    }
}

void WriteAtlasHtml(const fs::path& path, const std::vector<AtlasRow>& rows) {
    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();

    output << "<!doctype html>\n<html><head><meta charset=\"utf-8\">\n";
    output << "<title>Luminumbra Worldgen Atlas</title>\n";
    output << "<style>";
    output << "body{font-family:Segoe UI,Arial,sans-serif;margin:24px;background:#111;color:#eee;}";
    output << "table{border-collapse:collapse;width:100%;font-size:13px;}";
    output << "th,td{border:1px solid #333;padding:6px 8px;text-align:right;}";
    output << "th:first-child,td:first-child{text-align:left;}";
    output << "th{background:#222;}tr:nth-child(even){background:#181818;}";
    output << ".ok{color:#8ee28e}.warn{color:#ffd166}";
    output << "</style></head><body>\n";
    output << "<h1>Luminumbra Worldgen Atlas</h1>\n";
    output << "<p>Generated from authored presets. PPM layer images sit beside this report in the same artifact directory.</p>\n";
    output << "<table><thead><tr>";
    output << "<th>Preset</th><th>Chunk</th><th>Terrain Y</th><th>Spawn Y</th><th>Solid</th><th>Air</th>";
    output << "<th>Zero Edges</th><th>Verts</th><th>Tris</th><th>Degenerate</th><th>Bad Normals</th><th>SDF Error</th>";
    output << "</tr></thead><tbody>\n";

    for (const AtlasRow& row : rows) {
        const bool clean_mesh = row.snapshot.mesh.degenerate_triangles == 0u && row.snapshot.mesh.bad_vertex_normals == 0u;
        output << "<tr>";
        output << "<td>" << row.preset << "</td>";
        output << "<td>(" << row.chunk_coords.x << "," << row.chunk_coords.y << "," << row.chunk_coords.z << ")</td>";
        output << "<td>" << JsonNumber(row.terrain_height) << "</td>";
        output << "<td>" << JsonNumber(row.spawn_y) << "</td>";
        output << "<td>" << row.snapshot.sdf.solid_samples << "</td>";
        output << "<td>" << row.snapshot.sdf.air_samples << "</td>";
        output << "<td>" << row.snapshot.sdf.zero_crossing_edges << "</td>";
        output << "<td>" << row.snapshot.mesh.vertices << "</td>";
        output << "<td>" << row.snapshot.mesh.triangles << "</td>";
        output << "<td class=\"" << (clean_mesh ? "ok" : "warn") << "\">" << row.snapshot.mesh.degenerate_triangles << "</td>";
        output << "<td class=\"" << (clean_mesh ? "ok" : "warn") << "\">" << row.snapshot.mesh.bad_vertex_normals << "</td>";
        output << "<td>" << JsonNumber(row.snapshot.sampled_layers.max_sdf_sample_error) << "</td>";
        output << "</tr>\n";
    }

    output << "</tbody></table>\n</body></html>\n";
}

} // namespace

TEST(WorldGenLayerSnapshotTest, ExportsLayerMetricsAndImages) {
    const fs::path root = ArtifactRoot();
    fs::create_directories(root);

    TerrainGenParams terrain = TerrainLayerParams();
    TerrainGenParams island = terrain;
    island.island_mask_enabled = true;

    TerrainGenParams caves = CaveLayerParams();
    caves.island_mask_enabled = true;

    const LayerSnapshot base_snapshot = GenerateSnapshot("01_base_terrain", terrain, 1, false);
    const LayerSnapshot island_snapshot = GenerateSnapshot("02_island_mask", island, 1, false);
    const LayerSnapshot cave_surface_snapshot = GenerateSnapshot("03_caves_surface", caves, 1, false);
    const LayerSnapshot island_deep_snapshot = GenerateSnapshot("03a_island_deep", island, 1, false, kCaveChunkCoords);
    const LayerSnapshot cave_deep_snapshot = GenerateSnapshot("03b_caves_deep", caves, 1, false, kCaveChunkCoords);
    const LayerSnapshot cave_lod_snapshot = GenerateSnapshot("04_caves_lod4", caves, 4, false);
    const LayerSnapshot water_snapshot = GenerateSnapshot("05_submerged_water", WaterLayerParams(), 1, true);

    const std::vector<LayerSnapshot> snapshots{
        base_snapshot,
        island_snapshot,
        cave_surface_snapshot,
        island_deep_snapshot,
        cave_deep_snapshot,
        cave_lod_snapshot,
        water_snapshot,
    };

    const LayerDelta island_delta = CalculateDelta(base_snapshot, island_snapshot);
    const LayerDelta cave_delta = CalculateDelta(island_deep_snapshot, cave_deep_snapshot);
    const std::vector<LayerDelta> deltas{island_delta, cave_delta};

    for (const LayerSnapshot& snapshot : snapshots) {
        WriteSnapshotImages(root, snapshot);
    }
    WriteMetricsJson(root / "worldgen_layers.json", snapshots, deltas);

    EXPECT_GT(base_snapshot.sdf.solid_samples, 0u);
    EXPECT_GT(base_snapshot.sdf.air_samples, 0u);
    EXPECT_GT(base_snapshot.sdf.zero_crossing_edges, 0u);
    EXPECT_GT(base_snapshot.mesh.vertices, 0u);
    EXPECT_EQ(base_snapshot.mesh.invalid_indices, 0u);
    EXPECT_EQ(base_snapshot.mesh.degenerate_triangles, 0u);
    EXPECT_EQ(base_snapshot.mesh.bad_vertex_normals, 0u);
    EXPECT_LT(base_snapshot.sampled_layers.max_sdf_sample_error, 1.0e-4f);

    EXPECT_GT(island_delta.changed_height_samples, 0u);
    EXPECT_GT(island_delta.mean_abs_height_delta, 0.001);
    EXPECT_LT(island_snapshot.sampled_layers.max_sdf_sample_error, 1.0e-4f);

    EXPECT_GT(cave_delta.changed_sdf_samples, 0u);
    EXPECT_GT(cave_delta.sdf_sign_flips, 0u);
    EXPECT_GT(cave_deep_snapshot.mesh.vertices, 0u);
    EXPECT_EQ(cave_deep_snapshot.mesh.invalid_indices, 0u);
    EXPECT_EQ(cave_deep_snapshot.mesh.degenerate_triangles, 0u);
    EXPECT_EQ(cave_deep_snapshot.mesh.bad_vertex_normals, 0u);
    EXPECT_NE(cave_deep_snapshot.mesh.vertices, island_deep_snapshot.mesh.vertices);
    EXPECT_LT(cave_deep_snapshot.sampled_layers.max_sdf_sample_error, 1.0e-4f);
    EXPECT_LT(island_deep_snapshot.sampled_layers.max_sdf_sample_error, 1.0e-4f);

    EXPECT_GT(cave_surface_snapshot.mesh.vertices, 0u);
    EXPECT_EQ(cave_surface_snapshot.mesh.invalid_indices, 0u);
    EXPECT_EQ(cave_surface_snapshot.mesh.degenerate_triangles, 0u);
    EXPECT_EQ(cave_surface_snapshot.mesh.bad_vertex_normals, 0u);
    EXPECT_LT(cave_surface_snapshot.sampled_layers.max_sdf_sample_error, 1.0e-4f);

    EXPECT_GT(cave_lod_snapshot.mesh.vertices, 0u);
    EXPECT_LT(cave_lod_snapshot.mesh.vertices, cave_surface_snapshot.mesh.vertices);
    EXPECT_EQ(cave_lod_snapshot.mesh.invalid_indices, 0u);
    EXPECT_EQ(cave_lod_snapshot.mesh.degenerate_triangles, 0u);
    EXPECT_EQ(cave_lod_snapshot.mesh.bad_vertex_normals, 0u);
    EXPECT_LT(cave_lod_snapshot.sampled_layers.max_sdf_sample_error, 1.0e-4f);

    EXPECT_GT(water_snapshot.water_mesh.vertices, 0u);
    EXPECT_EQ(water_snapshot.water_mesh.invalid_indices, 0u);
    EXPECT_LT(water_snapshot.sampled_layers.max_sdf_sample_error, 1.0e-4f);
}

TEST(WorldGenLayerSnapshotTest, GeneratedSpawnCollisionPreventsFallThrough) {
    TerrainGenParams params;
    params.base_frequency = 0.01f;
    params.base_amplitude = 12.0f;
    params.octaves = 4;
    params.persistence = 0.5f;
    params.lacunarity = 2.0f;
    params.height_offset = 20.0f;
    params.caves_enabled = true;
    params.cave_frequency = 0.02f;

    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    const float spawn_x = 8.0f;
    const float spawn_z = 8.0f;
    const float terrain_height = world.GetTerrainHeightAt(spawn_x, spawn_z);
    const IVec3 surface_chunk = SHIELD_WorldSystem::world_to_chunk_coords(Vec3(spawn_x, terrain_height, spawn_z));
    Chunk chunk(surface_chunk);
    world.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);

    PhysicsSystem physics;
    physics.startup();
    physics.add_chunk_collision(chunk);
    physics.create_player_controller(glm::vec3(spawn_x, terrain_height + 10.0f, spawn_z));

    constexpr float dt = 1.0f / 60.0f;
    for (int frame = 0; frame < 300; ++frame) {
        physics.update_player(glm::vec3(0.0f), false, 0.0f, dt);
        physics.update(dt);
    }

    const glm::vec3 final_position = physics.get_player_position();
    EXPECT_GT(final_position.y, terrain_height - 2.0f);
    EXPECT_LT(final_position.y, terrain_height + 12.0f);
    physics.shutdown();
}

TEST(WorldGenLayerSnapshotTest, SpawnCollisionBootstrapPreparesWalkingStart) {
    TerrainGenParams params;
    params.base_frequency = 0.01f;
    params.base_amplitude = 12.0f;
    params.octaves = 4;
    params.persistence = 0.5f;
    params.lacunarity = 2.0f;
    params.height_offset = 20.0f;
    params.caves_enabled = true;
    params.cave_frequency = 0.02f;

    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);

    const float spawn_x = 0.0f;
    const float spawn_z = 0.0f;
    const float terrain_height = world.GetTerrainHeightAt(spawn_x, spawn_z);
    const glm::vec3 player_feet(spawn_x, terrain_height + 0.25f, spawn_z);

    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(world.EnsureCollisionReadyNear(Vec3(spawn_x, terrain_height + 1.95f, spawn_z), &physics, 1));
    physics.create_player_controller(player_feet);

    constexpr float dt = 1.0f / 60.0f;
    for (int frame = 0; frame < 300; ++frame) {
        physics.update_player(glm::vec3(0.0f), false, 0.0f, dt);
        physics.update(dt);
    }

    const glm::vec3 final_position = physics.get_player_position();
    EXPECT_GT(final_position.y, terrain_height - 1.0f);
    EXPECT_LT(final_position.y, terrain_height + 4.0f);
    physics.shutdown();
}

TEST(WorldGenLayerSnapshotTest, InitialChunkLoadListCoversSpawnSurfaceNeighborhood) {
    TerrainGenParams params;
    params.base_frequency = 0.01f;
    params.base_amplitude = 12.0f;
    params.octaves = 4;
    params.persistence = 0.5f;
    params.lacunarity = 2.0f;
    params.height_offset = 20.0f;
    params.caves_enabled = true;
    params.cave_frequency = 0.02f;

    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    const Vec3 spawn(8.0f, world.GetTerrainHeightAt(8.0f, 8.0f) + 1.95f, 8.0f);
    const std::vector<IVec3> initial_chunks = world.GetInitialChunkLoadList(spawn);

    // DELIBERATE expectation update (T-I3-2): the initial load list used to
    // emit exactly 3 chunks per column (center-point surface sample +-1, the
    // old 25*25*3 constant). It now emits the column's 5-point surface SPAN
    // (min..max chunk-Y across the column center + 4 footprint corners) plus
    // the same +-1 margin, so steep columns whose isosurface crosses a
    // chunk-Y border contribute extra cliff-wall chunks. The expected count
    // is derived from the same span math: span_size + 2 per column, with a
    // floor of the old 3-per-column emission.
    std::size_t expected_chunks = 0;
    const IVec3 spawn_chunk_for_count = SHIELD_WorldSystem::world_to_chunk_coords(spawn);
    for (int dz = -12; dz <= 12; ++dz) {
        for (int dx = -12; dx <= 12; ++dx) {
            const int chunk_x = spawn_chunk_for_count.x + dx;
            const int chunk_z = spawn_chunk_for_count.z + dz;
            const float base_x = static_cast<float>(chunk_x * CHUNK_SIZE_X);
            const float base_z = static_cast<float>(chunk_z * CHUNK_SIZE_Z);
            float min_height = std::numeric_limits<float>::max();
            float max_height = std::numeric_limits<float>::lowest();
            const std::array<std::pair<float, float>, 5> sample_points{{
                {base_x + CHUNK_SIZE_X * 0.5f, base_z + CHUNK_SIZE_Z * 0.5f},
                {base_x, base_z},
                {base_x + CHUNK_SIZE_X, base_z},
                {base_x, base_z + CHUNK_SIZE_Z},
                {base_x + CHUNK_SIZE_X, base_z + CHUNK_SIZE_Z},
            }};
            for (const auto& [px, pz] : sample_points) {
                const float h = world.GetTerrainHeightAt(px, pz);
                min_height = std::min(min_height, h);
                max_height = std::max(max_height, h);
            }
            const int span_min = SHIELD_WorldSystem::world_to_chunk_coords(Vec3(0.0f, min_height, 0.0f)).y;
            const int span_max = SHIELD_WorldSystem::world_to_chunk_coords(Vec3(0.0f, max_height, 0.0f)).y;
            expected_chunks += static_cast<std::size_t>(span_max - span_min + 3);
        }
    }
    EXPECT_EQ(initial_chunks.size(), expected_chunks);
    EXPECT_GE(initial_chunks.size(), 25u * 25u * 3u);

    std::unordered_set<ChunkID> loaded_ids;
    loaded_ids.reserve(initial_chunks.size());
    for (const IVec3& coords : initial_chunks) {
        loaded_ids.insert(Chunk::calculate_id(coords));
    }

    const IVec3 spawn_chunk = SHIELD_WorldSystem::world_to_chunk_coords(spawn);
    for (int dz = -12; dz <= 12; ++dz) {
        for (int dx = -12; dx <= 12; ++dx) {
            const int chunk_x = spawn_chunk.x + dx;
            const int chunk_z = spawn_chunk.z + dz;
            const float sample_x = static_cast<float>(chunk_x * CHUNK_SIZE_X) + CHUNK_SIZE_X * 0.5f;
            const float sample_z = static_cast<float>(chunk_z * CHUNK_SIZE_Z) + CHUNK_SIZE_Z * 0.5f;
            const float terrain_height = world.GetTerrainHeightAt(sample_x, sample_z);
            const int surface_y = SHIELD_WorldSystem::world_to_chunk_coords(Vec3(sample_x, terrain_height, sample_z)).y;

            EXPECT_TRUE(loaded_ids.find(Chunk::calculate_id(IVec3(chunk_x, surface_y, chunk_z))) != loaded_ids.end())
                << "missing surface chunk at " << chunk_x << "," << surface_y << "," << chunk_z;
        }
    }
}

TEST(WorldGenLayerSnapshotTest, SpawnReadyNeighborhoodMeshesBroadSurfaceArea) {
    TerrainGenParams params;
    params.base_frequency = 0.01f;
    params.base_amplitude = 12.0f;
    params.octaves = 4;
    params.persistence = 0.5f;
    params.lacunarity = 2.0f;
    params.height_offset = 20.0f;
    params.caves_enabled = true;
    params.cave_frequency = 0.02f;

    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    const Vec3 spawn(8.0f, world.GetTerrainHeightAt(8.0f, 8.0f) + 1.95f, 8.0f);

    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(world.EnsureSurfaceReadyNear(spawn, &physics, 12, 4));

    const std::vector<Chunk*> renderable_chunks = world.get_renderable_chunks();
    std::size_t mesh_chunks = 0;
    std::size_t total_triangles = 0;
    std::array<std::size_t, 3> lod_mesh_chunks{0u, 0u, 0u};
    for (const Chunk* chunk : renderable_chunks) {
        if (!chunk->mesh_vertices.empty() && !chunk->mesh_indices.empty()) {
            ++mesh_chunks;
            total_triangles += chunk->mesh_indices.size() / 3u;
            const int lod = std::clamp(chunk->current_lod.load(), 0, 2);
            ++lod_mesh_chunks[static_cast<std::size_t>(lod)];
        }
    }

    EXPECT_GE(mesh_chunks, 600u);
    EXPECT_GE(lod_mesh_chunks[0], 75u);
    EXPECT_GE(lod_mesh_chunks[1], 190u);
    EXPECT_GE(lod_mesh_chunks[2], 300u);
    EXPECT_GT(total_triangles, 5000u);
    physics.shutdown();
}

TEST(WorldGenLayerSnapshotTest, LodRemeshKeepsPreviousMeshRenderableWhilePending) {
    TerrainGenParams params;
    params.base_frequency = 0.01f;
    params.base_amplitude = 12.0f;
    params.octaves = 4;
    params.persistence = 0.5f;
    params.lacunarity = 2.0f;
    params.height_offset = 20.0f;
    params.caves_enabled = true;
    params.cave_frequency = 0.02f;

    Luminumbra::JobSystem jobs;
    jobs.startup();
    SHIELD_WorldSystem world(&jobs, nullptr, params, kSeed);
    const Vec3 spawn(8.0f, world.GetTerrainHeightAt(8.0f, 8.0f) + 1.95f, 8.0f);

    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(world.EnsureSurfaceReadyNear(spawn, &physics, 12, 4));

    Chunk* far_lod_chunk = nullptr;
    for (Chunk* chunk : world.get_renderable_chunks()) {
        if (chunk->current_lod.load() == 2 && !chunk->mesh_vertices.empty() && !chunk->mesh_indices.empty()) {
            far_lod_chunk = chunk;
            break;
        }
    }
    ASSERT_NE(far_lod_chunk, nullptr);
    const ChunkID target_id = far_lod_chunk->get_id();
    const u32 previous_mesh_version = far_lod_chunk->mesh_version.load();
    const IVec3 target_coords = far_lod_chunk->get_coords();
    const Vec3 target_center =
        (Vec3(target_coords) + 0.5f) * Vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);

    // Hole-filling candidates (chunks with no active mesh) are deliberately
    // scheduled ahead of LOD-improvement remeshes, and each update only
    // dispatches a bounded batch, so a single update is not guaranteed to
    // schedule this chunk. Pump the streaming update until the LOD0 request
    // lands; the invariant under test is that the previous mesh stays
    // renderable the entire time.
    entt::registry registry;
    Chunk* target_after_request = nullptr;
    bool lod0_requested_or_active = false;
    for (int iteration = 0; iteration < 600 && !lod0_requested_or_active; ++iteration) {
        world.update(registry, target_center, &physics);

        target_after_request = nullptr;
        for (Chunk* chunk : world.get_renderable_chunks()) {
            if (chunk->get_id() == target_id) {
                target_after_request = chunk;
                break;
            }
        }
        ASSERT_NE(target_after_request, nullptr);
        ASSERT_FALSE(target_after_request->mesh_vertices.empty());
        ASSERT_FALSE(target_after_request->mesh_indices.empty());

        lod0_requested_or_active =
            target_after_request->pending_lod.load() == 0 ||
            target_after_request->current_lod.load() == 0;
        if (!lod0_requested_or_active) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    ASSERT_NE(target_after_request, nullptr);
    EXPECT_TRUE(lod0_requested_or_active);
    EXPECT_FALSE(target_after_request->mesh_vertices.empty());
    EXPECT_FALSE(target_after_request->mesh_indices.empty());
    EXPECT_GE(target_after_request->mesh_version.load(), previous_mesh_version);

    physics.shutdown();
}

TEST(WorldGenLayerSnapshotTest, VerticalUnloadExemptsColumnSurfaceSpanChunks) {
    // T-I3-2 (F4): the camera-relative vertical unload test evicted surface
    // chunks of tall peaks (> 10 chunk-Ys above the camera), which the
    // surface scan immediately re-candidated - a churn loop that left holes
    // on mountain summits. Chunks inside their column's surface band must be
    // exempt from the vertical test; chunks far off the surface still unload.
    TerrainGenParams params;
    params.base_frequency = 0.01f;
    params.base_amplitude = 0.0f;   // flat world ...
    params.height_offset = 200.0f;  // ... with its surface in chunk-Y 12
    params.caves_enabled = false;

    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);

    // Camera in the "valley" at y=0: the surface chunk (0, 12, 0) sits
    // d.y = 12 above the camera chunk, beyond UNLOAD_DISTANCE_UP (10).
    const Vec3 camera(8.0f, 0.0f, 8.0f);
    const IVec3 surface_coords(0, 12, 0);
    const IVec3 sky_coords(0, 20, 0); // far above the surface band: evictable

    ASSERT_TRUE(world.adopt_streamed_chunk(std::make_shared<Chunk>(surface_coords)));
    ASSERT_TRUE(world.adopt_streamed_chunk(std::make_shared<Chunk>(sky_coords)));

    entt::registry registry;
    for (int i = 0; i < 8; ++i) {
        world.update(registry, camera, nullptr);
    }

    EXPECT_NE(world.find_streamed_chunk(surface_coords), nullptr)
        << "surface-span chunk above the camera must survive the vertical unload test";
    EXPECT_EQ(world.find_streamed_chunk(sky_coords), nullptr)
        << "chunk far above the surface band must still be vertically evicted";
}

TEST(WorldGenLayerSnapshotTest, MountainsSurfaceSpanWantedSetStaysUnderChunkBudget) {
    // T-I3-2 budget proof: the steady-state activation wanted set with
    // 5-point surface spans must fit the 8192 active-chunk budget on the
    // worst-case shipped preset (mountains: amplitude 120, 6 octaves). The
    // count mirrors update_chunk_activation's candidate rule: every chunk-Y
    // in the column span at every ring, plus the +-1 stack inside ring 12.
    TerrainGenParams params;
    params.base_frequency = 0.008f;
    params.base_amplitude = 120.0f;
    params.octaves = 6;
    params.persistence = 0.65f;
    params.lacunarity = 2.2f;
    params.height_offset = 20.0f;
    params.caves_enabled = true;
    params.cave_frequency = 0.03f;

    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);

    auto wanted_chunks_at_radius = [&world](int radius) {
        std::size_t wanted = 0;
        for (int dz = -radius; dz <= radius; ++dz) {
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx * dx + dz * dz > radius * radius) {
                    continue;
                }
                float min_height = std::numeric_limits<float>::max();
                float max_height = std::numeric_limits<float>::lowest();
                const float base_x = static_cast<float>(dx * CHUNK_SIZE_X);
                const float base_z = static_cast<float>(dz * CHUNK_SIZE_Z);
                const std::array<std::pair<float, float>, 5> sample_points{{
                    {base_x + CHUNK_SIZE_X * 0.5f, base_z + CHUNK_SIZE_Z * 0.5f},
                    {base_x, base_z},
                    {base_x + CHUNK_SIZE_X, base_z},
                    {base_x, base_z + CHUNK_SIZE_Z},
                    {base_x + CHUNK_SIZE_X, base_z + CHUNK_SIZE_Z},
                }};
                for (const auto& [px, pz] : sample_points) {
                    const float h = world.GetTerrainHeightAt(px, pz);
                    min_height = std::min(min_height, h);
                    max_height = std::max(max_height, h);
                }
                const int span_min = SHIELD_WorldSystem::world_to_chunk_coords(Vec3(0.0f, min_height, 0.0f)).y;
                const int span_max = SHIELD_WorldSystem::world_to_chunk_coords(Vec3(0.0f, max_height, 0.0f)).y;
                const int ring = std::max(std::abs(dx), std::abs(dz));
                wanted += static_cast<std::size_t>(span_max - span_min + 1);
                if (ring <= 12) {
                    wanted += 2; // +-1 vertical stack inside the mid ring
                }
            }
        }
        return wanted;
    };

    const std::size_t wanted_full_radius = wanted_chunks_at_radius(RENDER_DISTANCE);
    const std::size_t wanted_radius_24 = wanted_chunks_at_radius(24);
    const std::size_t wanted_radius_20 = wanted_chunks_at_radius(20);
    const std::size_t wanted_player_core = wanted_chunks_at_radius(12);
    std::cout << "[ SPANBUDGET ] mountains wanted set: radius " << RENDER_DISTANCE
              << " -> " << wanted_full_radius << ", radius 24 -> " << wanted_radius_24
              << ", radius 20 -> " << wanted_radius_20
              << ", radius 12 (player-view core) -> " << wanted_player_core
              << " chunks (budget 8192)" << std::endl;

    // The activation pass truncates the SORTED candidate list at the budget,
    // so active chunks can never exceed 8192 and any trim lands on the
    // farthest rim. These asserts pin the budget headroom where it matters:
    // the pressure-throttled radius (20, the radius streaming falls back to
    // under load) must fit entirely, and the player-view core (radius 12,
    // the LOD0/collision neighborhood the PlayerView gate measures) must
    // leave generous headroom. The full radius-32 mountains wanted set
    // (~13.5k) deliberately exceeds the budget - the trim is the documented
    // trade until the far-LOD region store (T8/T9) replaces live chunks
    // beyond the near field.
    EXPECT_LT(wanted_radius_20, 8192u);
    EXPECT_LT(wanted_player_core, 4096u);
}

TEST(WorldGenLayerSnapshotTest, AuthoredPresetAtlasHasSaneSpawnAndCleanTopology) {
    const fs::path atlas_root = ArtifactRoot() / "atlas";
    fs::create_directories(atlas_root);

    const fs::path preset_root = SourceRoot() / "worlds/atlas/presets";
    ASSERT_TRUE(fs::exists(preset_root)) << preset_root.string();

    std::vector<AtlasRow> rows;
    for (const fs::directory_entry& entry : fs::directory_iterator(preset_root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }

        const std::string preset_name = entry.path().stem().string();
        const TerrainGenParams params = LoadPresetParams(entry.path());
        SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);

        const float sample_x = 8.0f;
        const float sample_z = 8.0f;
        const WorldGenLayerSample sample = world.SampleWorldGenLayers(Vec3(sample_x, 0.0f, sample_z));
        const float terrain_height = world.GetTerrainHeightAt(sample_x, sample_z);
        const float spawn_y = terrain_height + 1.95f;
        const IVec3 surface_chunk = SHIELD_WorldSystem::world_to_chunk_coords(Vec3(sample_x, terrain_height, sample_z));

        LayerSnapshot snapshot = GenerateSnapshot("atlas_" + preset_name, params, 1, false, surface_chunk);
        WriteSnapshotImages(atlas_root, snapshot);

        EXPECT_TRUE(std::isfinite(terrain_height)) << preset_name;
        EXPECT_TRUE(std::isfinite(spawn_y)) << preset_name;
        EXPECT_NEAR(terrain_height, sample.final_height, 1.0e-4f) << preset_name;
        EXPECT_GT(spawn_y, -512.0f) << preset_name;
        EXPECT_LT(spawn_y, 512.0f) << preset_name;
        EXPECT_GT(snapshot.sdf.solid_samples, 0u) << preset_name;
        EXPECT_GT(snapshot.sdf.air_samples, 0u) << preset_name;
        EXPECT_GT(snapshot.sdf.zero_crossing_edges, 0u) << preset_name;
        EXPECT_GT(snapshot.mesh.vertices, 0u) << preset_name;
        EXPECT_EQ(snapshot.mesh.invalid_indices, 0u) << preset_name;
        EXPECT_EQ(snapshot.mesh.degenerate_triangles, 0u) << preset_name;
        EXPECT_EQ(snapshot.mesh.bad_vertex_normals, 0u) << preset_name;
        EXPECT_LT(snapshot.sampled_layers.max_sdf_sample_error, 1.0e-4f) << preset_name;

        rows.push_back(AtlasRow{preset_name, surface_chunk, terrain_height, spawn_y, std::move(snapshot)});
    }

    ASSERT_GE(rows.size(), 5u);
    std::sort(rows.begin(), rows.end(), [](const AtlasRow& lhs, const AtlasRow& rhs) {
        return lhs.preset < rhs.preset;
    });
    WriteAtlasHtml(atlas_root / "worldgen_atlas.html", rows);
}
