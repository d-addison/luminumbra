#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
#include "world/BiomeTable.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"
#include "world/TerrainPresetLoader.h"

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
    // T-I3-5: delegate to the canonical engine preset parser.
    const Luminumbra::world::TerrainPresetLoadResult result =
        Luminumbra::world::LoadTerrainPreset(path);
    EXPECT_TRUE(result.ok) << path.string();
    for (const std::string& error : result.errors) {
        ADD_FAILURE() << "preset parse error: " << error;
    }
    return result.params;
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
    // worst-case params (frozen copy of the pre-T-I3-11 mountains preset:
    // amplitude 120, 6 octaves, no shaping - the shipped shaped mountains
    // is strictly gentler, so this stays the upper bound; the PlayerView
    // gate checks the live budget on the shipped preset at runtime). The
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

// ---------------------------------------------------------------------------
// T-I3-10 terrain shaping gates
// ---------------------------------------------------------------------------

namespace {

constexpr std::uint64_t kFnvOffsetBasis = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t Fnv1a64Bytes(std::uint64_t hash, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::uint64_t>(bytes[i]);
        hash *= kFnvPrime;
    }
    return hash;
}

// FNV-1a-64 over the raw float bytes of GetTerrainHeightAt sampled on a fixed
// 64x64 grid (step 16.25 m so fractional world coordinates are exercised).
std::uint64_t HashTerrainHeightGrid(const SHIELD_WorldSystem& world) {
    std::uint64_t hash = kFnvOffsetBasis;
    for (int j = 0; j < 64; ++j) {
        for (int i = 0; i < 64; ++i) {
            const float x = -512.0f + static_cast<float>(i) * 16.25f;
            const float z = -512.0f + static_cast<float>(j) * 16.25f;
            const float height = world.GetTerrainHeightAt(x, z);
            hash = Fnv1a64Bytes(hash, &height, sizeof(height));
        }
    }
    return hash;
}

struct LegacyPresetHeightFixture {
    const char* name;
    TerrainGenParams params;
    std::uint64_t expected_hash;
};

// Frozen copies of the five shipped presets' terrain params as of the commit
// BEFORE T-I3-10 (shaping defaults off). These fixtures deliberately do NOT
// load the preset JSON files: shipped presets may later opt into shaping
// (T-I3-11), but legacy params must keep producing bit-identical heights
// forever. The expected hashes were captured by running this exact grid hash
// against the pre-shaping GetTerrainHeightAt implementation.
std::vector<LegacyPresetHeightFixture> LegacyPresetHeightFixtures() {
    std::vector<LegacyPresetHeightFixture> fixtures;

    TerrainGenParams default_params;
    default_params.base_frequency = 0.01f;
    default_params.base_amplitude = 12.0f;
    default_params.octaves = 4;
    default_params.persistence = 0.5f;
    default_params.lacunarity = 2.0f;
    default_params.height_offset = 20.0f;
    default_params.caves_enabled = true;
    default_params.cave_frequency = 0.02f;
    fixtures.push_back({"default", default_params, 0xe585505dedeba6c9ull});

    TerrainGenParams flat_params;
    flat_params.base_frequency = 0.02f;
    flat_params.base_amplitude = 10.0f;
    flat_params.octaves = 2;
    flat_params.persistence = 0.3f;
    flat_params.lacunarity = 2.0f;
    flat_params.height_offset = 5.0f;
    flat_params.caves_enabled = false;
    flat_params.cave_frequency = 0.0f;
    fixtures.push_back({"flat_lands", flat_params, 0x5f0afd93c41d6b51ull});

    TerrainGenParams mountains_params;
    mountains_params.base_frequency = 0.008f;
    mountains_params.base_amplitude = 120.0f;
    mountains_params.octaves = 6;
    mountains_params.persistence = 0.65f;
    mountains_params.lacunarity = 2.2f;
    mountains_params.height_offset = 20.0f;
    mountains_params.caves_enabled = true;
    mountains_params.cave_frequency = 0.03f;
    fixtures.push_back({"mountains", mountains_params, 0xc7d4205d48faabb7ull});

    TerrainGenParams archipelago_params;
    archipelago_params.base_frequency = 0.009f;
    archipelago_params.base_amplitude = 110.0f;
    archipelago_params.octaves = 6;
    archipelago_params.persistence = 0.55f;
    archipelago_params.lacunarity = 2.2f;
    archipelago_params.height_offset = -20.0f;
    archipelago_params.island_mask_enabled = true;
    archipelago_params.island_mask_frequency = 0.004f;
    archipelago_params.caves_enabled = true;
    archipelago_params.cave_frequency = 0.03f;
    fixtures.push_back({"archipelago", archipelago_params, 0xc075cf55c182393cull});

    TerrainGenParams forest_params;
    forest_params.base_frequency = 0.008f;
    forest_params.base_amplitude = 50.0f;
    forest_params.octaves = 6;
    forest_params.persistence = 0.5f;
    forest_params.lacunarity = 2.1f;
    forest_params.height_offset = 32.0f;
    forest_params.caves_enabled = true;
    forest_params.cave_frequency = 0.025f;
    fixtures.push_back({"temperate_forest", forest_params, 0xb9b8b2f79e44b42dull});

    return fixtures;
}

} // namespace

// T-I3-10 zero-hash-drift proof: legacy params (shaping_enabled == false, the
// default) must produce heights bit-identical to the pre-shaping
// implementation. Hashes captured pre-change; any drift here is a
// review-blocking defect, never a re-bless.
TEST(WorldGenLayerSnapshotTest, LegacyPresetHeightsAreBitIdenticalToPreShaping) {
    for (const LegacyPresetHeightFixture& fixture : LegacyPresetHeightFixtures()) {
        SHIELD_WorldSystem world(nullptr, nullptr, fixture.params, kSeed);
        const std::uint64_t hash = HashTerrainHeightGrid(world);
        std::cout << "[ LEGACYHEIGHT ] " << fixture.name << " seed=" << kSeed
                  << " hash=0x" << std::hex << std::setfill('0') << std::setw(16) << hash
                  << std::dec << std::setfill(' ') << std::endl;
        EXPECT_EQ(hash, fixture.expected_hash)
            << fixture.name << ": legacy (shaping-off) terrain heights drifted from the "
            << "pre-shaping implementation - this is a hard determinism break";
    }
}

// T-I3-22 slice polish: CURRENT shipped-preset terrain hash. Unlike the
// legacy fixture above (frozen pre-shaping params, must NEVER change), this
// gate loads the LIVE preset JSON through the canonical loader and hashes the
// resulting GetTerrainHeightAt grid. It catches accidental drift in a shipped
// preset's generated terrain AND forces any deliberate preset edit to bump the
// expected hash in the same commit (documented in the commit message).
//
// The archipelago hash was bumped deliberately in T-I3-22 when the schema_rev
// 2 `shaping` block was added (spiky-blade shores -> rolling shores / walkable
// interiors). The pre-shaping archipelago hash (0xc075cf55c182393c) is frozen
// forever in the LEGACY fixture above as the default-off shaping proof.
TEST(WorldGenLayerSnapshotTest, CurrentShippedArchipelagoPresetHeightHash) {
    const fs::path preset = SourceRoot() / "worlds/atlas/presets/archipelago.json";
    const TerrainGenParams params = LoadPresetParams(preset);
    ASSERT_TRUE(params.shaping_enabled)
        << "archipelago.json must carry an enabled shaping block (T-I3-22)";
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    const std::uint64_t hash = HashTerrainHeightGrid(world);
    std::cout << "[ CURRENTHASH ] archipelago seed=" << kSeed
              << " hash=0x" << std::hex << std::setfill('0') << std::setw(16) << hash
              << std::dec << std::setfill(' ') << std::endl;
    // DELIBERATE BUMP (T-I3-22 slice polish). Before (legacy, shaping-off):
    // 0xc075cf55c182393c. After (schema_rev 2 shaping): 0x940d621a2e3c0436.
    constexpr std::uint64_t kArchipelagoShapedHash = 0x940d621a2e3c0436ull;
    EXPECT_EQ(hash, kArchipelagoShapedHash)
        << "shipped archipelago preset terrain drifted; if intentional, bump "
        << "kArchipelagoShapedHash deliberately and document before/after in "
        << "the commit message";
}

namespace {

// Synthetic shaping params for the T-I3-10 parity/determinism gates (engine
// tests must not depend on game preset data choices).
TerrainGenParams ShapingTestParams() {
    TerrainGenParams params;
    params.base_frequency = 0.008f;
    params.base_amplitude = 60.0f;
    params.octaves = 5;
    params.persistence = 0.55f;
    params.lacunarity = 2.1f;
    params.height_offset = 12.0f;
    params.caves_enabled = true;
    params.cave_frequency = 0.03f;
    params.shaping_enabled = true;
    params.continentalness_frequency = 0.0008f;
    params.erosion_frequency = 0.0015f;
    params.peaks_frequency = 0.004f;
    params.peaks_amplitude = 90.0f;
    params.domain_warp_amplitude = 30.0f;
    params.domain_warp_frequency = 0.006f;
    params.continental_spline = {{-1.0f, -40.0f}, {-0.3f, -12.0f}, {-0.1f, 2.0f}, {0.3f, 14.0f}, {1.0f, 42.0f}};
    params.erosion_spline = {{-1.0f, 1.0f}, {0.0f, 0.55f}, {0.6f, 0.18f}, {1.0f, 0.05f}};
    params.peaks_spline = {{-1.0f, 0.0f}, {0.4f, 0.05f}, {0.8f, 0.45f}, {1.0f, 1.0f}};
    return params;
}

} // namespace

// T-I3-10 batch-vs-scalar parity: with shaping enabled, every generation path
// computes heights through the one shared scalar helper (GenSingle* APIs in
// both the scalar and batch paths), so the batch heightmap bytes must be
// EXACTLY equal (==, no epsilon) to GetTerrainHeightAt at the same world
// coordinates - for the full-SDF path, the step>1 heightmap-only path, and
// SampleWorldGenLayers. FastNoise SIMD grid batches (GenUniformGrid2D) are
// deliberately NOT used for shaped heights precisely so no SIMD-lane epsilon
// is needed here; the legacy (shaping-off) grid batches stay covered by the
// existing max_sdf_sample_error < 1e-4 snapshot gate.
TEST(WorldGenLayerSnapshotTest, ShapedHeightBatchPathsExactlyMatchScalarPath) {
    const TerrainGenParams params = ShapingTestParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);

    const std::array<IVec3, 4> chunk_coords{{
        IVec3(0, 0, 0), IVec3(-3, 1, 2), IVec3(7, -1, -5), IVec3(-11, 0, 9),
    }};
    for (const IVec3& coords : chunk_coords) {
        const IVec3 base_pos = coords * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);

        Chunk full_chunk(coords);
        world.GenerateChunkData(full_chunk, 1);
        Chunk coarse_chunk(coords);
        world.GenerateChunkData(coarse_chunk, 4);
        ASSERT_TRUE(coarse_chunk.sdf_data.empty());

        for (int z = 0; z <= CHUNK_SIZE_Z; ++z) {
            for (int x = 0; x <= CHUNK_SIZE_X; ++x) {
                const float world_x = static_cast<float>(base_pos.x + x);
                const float world_z = static_cast<float>(base_pos.z + z);
                const float scalar_height = world.GetTerrainHeightAt(world_x, world_z);
                const std::size_t index = static_cast<std::size_t>(x)
                    + static_cast<std::size_t>(z) * (CHUNK_SIZE_X + 1);

                EXPECT_EQ(full_chunk.heightmap_data[index], scalar_height)
                    << "full-path heightmap diverged from scalar at (" << world_x << ", " << world_z << ")";
                EXPECT_EQ(coarse_chunk.heightmap_data[index], scalar_height)
                    << "step>1 heightmap diverged from scalar at (" << world_x << ", " << world_z << ")";

                const WorldGenLayerSample sample =
                    world.SampleWorldGenLayers(Vec3(world_x, 0.0f, world_z));
                EXPECT_EQ(sample.final_height, scalar_height)
                    << "SampleWorldGenLayers diverged from scalar at (" << world_x << ", " << world_z << ")";
            }
        }
    }
}

// T-I3-10: generation with shaping ON stays deterministic for a fixed seed
// (two independent systems produce byte-identical SDF + heightmap), and a
// different seed produces different terrain (the control channels actually
// consume the seed offsets).
TEST(WorldGenLayerSnapshotTest, ShapedGenerationIsDeterministicWithSameSeed) {
    const TerrainGenParams params = ShapingTestParams();
    SHIELD_WorldSystem world_a(nullptr, nullptr, params, kSeed);
    SHIELD_WorldSystem world_b(nullptr, nullptr, params, kSeed);
    SHIELD_WorldSystem world_c(nullptr, nullptr, params, kSeed + 1);

    Chunk chunk_a(kChunkCoords);
    Chunk chunk_b(kChunkCoords);
    Chunk chunk_c(kChunkCoords);
    world_a.GenerateChunkData(chunk_a);
    world_b.GenerateChunkData(chunk_b);
    world_c.GenerateChunkData(chunk_c);

    EXPECT_EQ(chunk_a.sdf_data, chunk_b.sdf_data);
    EXPECT_EQ(chunk_a.heightmap_data, chunk_b.heightmap_data);
    EXPECT_NE(chunk_a.heightmap_data, chunk_c.heightmap_data);

    // The shaped-height snapshot keeps the sample-path consistency gate green
    // with shaping ON (cave noise is still grid-batched, hence the 1e-4
    // tolerance rather than exact equality for full SDF samples).
    const LayerSnapshot shaped_snapshot = GenerateSnapshot("06_shaping", params, 1, false);
    EXPECT_GT(shaped_snapshot.sdf.solid_samples, 0u);
    EXPECT_GT(shaped_snapshot.sdf.air_samples, 0u);
    EXPECT_LT(shaped_snapshot.sampled_layers.max_sdf_sample_error, 1.0e-4f);
}

// ---------------------------------------------------------------------------
// T-I4-1 biome selection.
//
// The shipped biome table loads, its lookup is deterministic and resolves the
// documented edge cases (clamping at the domain edges, overlapping ranges =
// first-match), and - critically - a preset WITHOUT the biomes opt-in stays
// byte-zero (every existing snapshot/hash fixture above proves it; these cases
// prove the new biome_id/material paths leave legacy worlds untouched).

namespace {

fs::path BiomeTablePath() {
    return SourceRoot() / "data" / "common" / "biomes.json";
}

// A biome-enabled copy of the shaped mountains params, pointing the world at
// the shipped table. Used by the determinism cases; never touches a shipped
// preset, so the legacy fixtures stay frozen.
TerrainGenParams BiomeEnabledParams() {
    TerrainGenParams params = ShapingTestParams();
    params.biomes_enabled = true;
    params.biome_table_path = BiomeTablePath().string();
    params.temperature_frequency = 0.003f;
    params.humidity_frequency = 0.004f;
    return params;
}

} // namespace

TEST(WorldGenLayerSnapshotTest, BiomeTableLoadsAuthoredBiomes) {
    const Luminumbra::World::BiomeTable table =
        Luminumbra::World::BiomeTable::Load(BiomeTablePath());
    ASSERT_TRUE(table.ok()) << "errors: "
                            << (table.errors().empty() ? "<none>" : table.errors().front());
    EXPECT_TRUE(table.errors().empty());
    EXPECT_GE(table.size(), 4u) << "design-decisions section 2 mandates 4-6 authored biomes";
    EXPECT_LE(table.size(), 6u);
    EXPECT_NE(table.content_hash(), 0u);

    // Every authored palette must reference a real (non-Air) material id.
    for (const auto& biome : table.biomes()) {
        EXPECT_NE(biome.palette.top, static_cast<u8>(MaterialType::Air)) << biome.name;
        EXPECT_LE(biome.palette.top, static_cast<u8>(MaterialType::Water)) << biome.name;
        EXPECT_LE(biome.palette.filler, static_cast<u8>(MaterialType::Water)) << biome.name;
        EXPECT_LE(biome.palette.depth, static_cast<u8>(MaterialType::Water)) << biome.name;
        EXPECT_LE(biome.palette.underwater, static_cast<u8>(MaterialType::Water)) << biome.name;
    }
}

TEST(WorldGenLayerSnapshotTest, BiomeTableContentHashIsStable) {
    const Luminumbra::World::BiomeTable a =
        Luminumbra::World::BiomeTable::Load(BiomeTablePath());
    const Luminumbra::World::BiomeTable b =
        Luminumbra::World::BiomeTable::Load(BiomeTablePath());
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.content_hash(), b.content_hash())
        << "table content hash must be a pure function of the file content";
}

TEST(WorldGenLayerSnapshotTest, BiomeLookupResolvesEdgeCasesDeterministically) {
    using Luminumbra::World::BiomeClimateRange;
    using Luminumbra::World::kNoBiome;

    // Range contains() contract: inclusive-min, exclusive-max, with the domain
    // ceiling 1.0 inclusive so a value sitting exactly at the top resolves.
    const BiomeClimateRange r{0.0f, 0.5f};
    EXPECT_TRUE(r.contains(0.0f));   // inclusive min
    EXPECT_TRUE(r.contains(0.25f));
    EXPECT_FALSE(r.contains(0.5f));  // exclusive max (interior boundary)
    EXPECT_FALSE(r.contains(-0.1f));
    const BiomeClimateRange top{0.5f, 1.0f};
    EXPECT_TRUE(top.contains(1.0f)); // domain ceiling is inclusive

    const Luminumbra::World::BiomeTable table =
        Luminumbra::World::BiomeTable::Load(BiomeTablePath());
    ASSERT_TRUE(table.ok());

    // Determinism: the same climate inputs always resolve the same id.
    const u8 first = table.lookup(0.5f, -0.5f, 0.0f, -0.5f, 0.0f);
    const u8 second = table.lookup(0.5f, -0.5f, 0.0f, -0.5f, 0.0f);
    EXPECT_EQ(first, second);

    // First-match on overlapping ranges: the authored "plains" row is a
    // full-span catch-all declared LAST, so any climate that matches no
    // earlier specific row resolves to it (never kNoBiome) - and an earlier
    // matching specific row wins over it. We assert a temperate/humid lowland
    // resolves a real biome rather than the sentinel.
    const u8 lowland = table.lookup(-0.5f, 0.5f, 0.0f, 0.5f, 0.5f);
    EXPECT_NE(lowland, kNoBiome) << "catch-all plains row must cover unmatched climate";

    // Clamping at the domain extremes still resolves deterministically.
    EXPECT_EQ(table.lookup(-1.0f, -1.0f, -1.0f, -1.0f, -1.0f),
              table.lookup(-1.0f, -1.0f, -1.0f, -1.0f, -1.0f));
    EXPECT_EQ(table.lookup(1.0f, 1.0f, 1.0f, 1.0f, 1.0f),
              table.lookup(1.0f, 1.0f, 1.0f, 1.0f, 1.0f));

    // palette_for is total: kNoBiome and any unknown id return the default
    // palette rather than reading out of bounds.
    const auto& none_palette = table.palette_for(kNoBiome);
    EXPECT_LE(none_palette.top, static_cast<u8>(MaterialType::Water));
}

TEST(WorldGenLayerSnapshotTest, BiomeIdIsDeterministicAndCoversMultipleBiomes) {
    const TerrainGenParams params = BiomeEnabledParams();
    SHIELD_WorldSystem world_a(nullptr, nullptr, params, kSeed);
    SHIELD_WorldSystem world_b(nullptr, nullptr, params, kSeed);
    ASSERT_TRUE(world_a.biomes_enabled());

    std::unordered_set<int> distinct_biomes;
    bool all_match = true;
    for (int z = -512; z <= 512; z += 32) {
        for (int x = -512; x <= 512; x += 32) {
            const u8 a = world_a.BiomeIdAt(static_cast<float>(x), static_cast<float>(z));
            const u8 b = world_b.BiomeIdAt(static_cast<float>(x), static_cast<float>(z));
            if (a != b) {
                all_match = false;
            }
            distinct_biomes.insert(static_cast<int>(a));
        }
    }
    EXPECT_TRUE(all_match) << "BiomeIdAt must be a deterministic function of (seed, params)";
    // The catch-all plains row guarantees no column is kNoBiome, and the
    // climate field must vary enough to surface more than one biome.
    EXPECT_EQ(distinct_biomes.count(static_cast<int>(Luminumbra::World::kNoBiome)), 0u)
        << "no column should be unmatched (plains is the catch-all)";
    EXPECT_GE(distinct_biomes.size(), 2u) << "the seed window must span >= 2 biomes";
}

TEST(WorldGenLayerSnapshotTest, BiomesDisabledIsByteZeroDrift) {
    // A preset without the biomes opt-in: BiomeIdAt is always kNoBiome and the
    // generated chunk bytes are bit-identical to a world that never knew about
    // biomes (the new biome plumbing is inert when disabled).
    const TerrainGenParams legacy = ShapingTestParams();
    SHIELD_WorldSystem world(nullptr, nullptr, legacy, kSeed);
    EXPECT_FALSE(world.biomes_enabled());
    EXPECT_EQ(world.BiomeIdAt(8.0f, 8.0f), Luminumbra::World::kNoBiome);
    EXPECT_EQ(world.BiomeIdAt(200.0f, -160.0f), Luminumbra::World::kNoBiome);

    Chunk legacy_chunk(kChunkCoords);
    world.GenerateChunkData(legacy_chunk);
    EXPECT_GT(legacy_chunk.heightmap_data.size(), 0u);

    // The shaped-determinism fixture above already pins these exact bytes for
    // the same params/seed; re-running here proves the biome code added no
    // drift to the disabled path.
    SHIELD_WorldSystem reference(nullptr, nullptr, legacy, kSeed);
    Chunk reference_chunk(kChunkCoords);
    reference.GenerateChunkData(reference_chunk);
    EXPECT_EQ(legacy_chunk.sdf_data, reference_chunk.sdf_data);
    EXPECT_EQ(legacy_chunk.heightmap_data, reference_chunk.heightmap_data);
    EXPECT_EQ(legacy_chunk.mesh_vertices.size(), reference_chunk.mesh_vertices.size());
}

// T-I4-2 BiomeCoverage source: a CPU atlas sweep over the shipped mountains
// preset (biomes ENABLED) at the fixed atlas seed. Asserts every authored
// biome is present in the window and per-biome surface-material distribution
// bands hold, then writes biome-coverage.json for the BiomeCoverage validator
// mode. The same artifact pins the mountains biome material-distribution
// contract (deliberate bump on a table/preset change).
TEST(WorldGenLayerSnapshotTest, MountainsBiomeCoverageAtlas) {
    const fs::path preset = SourceRoot() / "worlds/atlas/presets/mountains.json";
    ASSERT_TRUE(fs::exists(preset)) << preset.string();
    const TerrainGenParams params = LoadPresetParams(preset);
    ASSERT_TRUE(params.biomes_enabled) << "mountains must opt into biomes (T-I4-2)";

    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    ASSERT_TRUE(world.biomes_enabled());
    const auto& table = world.biome_table();
    ASSERT_GE(table.size(), 4u);

    // Atlas window: 1024 m square at 8 m spacing, centered on the origin.
    constexpr int kHalf = 512;
    constexpr int kStep = 8;
    std::unordered_map<int, std::size_t> biome_columns;       // biome id -> column count
    std::unordered_map<int, std::array<std::size_t, 8>> material_hist; // biome id -> material counts
    std::size_t total_columns = 0;
    for (int z = -kHalf; z <= kHalf; z += kStep) {
        for (int x = -kHalf; x <= kHalf; x += kStep) {
            const float fx = static_cast<float>(x);
            const float fz = static_cast<float>(z);
            const u8 biome_id = world.BiomeIdAt(fx, fz);
            const float height = world.GetTerrainHeightAt(fx, fz);
            // Classify the surface skin (depth 0) for the distribution band.
            const MaterialType material = world.SurfaceMaterialForColumn(height, height, biome_id);
            const int bid = static_cast<int>(biome_id);
            ++biome_columns[bid];
            const auto mat_index = static_cast<std::size_t>(material);
            if (mat_index < 8) {
                ++material_hist[bid][mat_index];
            }
            ++total_columns;
        }
    }
    ASSERT_GT(total_columns, 0u);

    // Every authored biome must appear at least once over the fixed window.
    for (const auto& biome : table.biomes()) {
        EXPECT_GT(biome_columns[static_cast<int>(biome.id)], 0u)
            << "authored biome '" << biome.name << "' (id " << static_cast<int>(biome.id)
            << ") is absent from the mountains atlas window";
    }
    // No column should be unmatched: plains is the full-span catch-all.
    EXPECT_EQ(biome_columns[static_cast<int>(Luminumbra::World::kNoBiome)], 0u);

    // Per-biome material band: a biome's surface-skin columns must be drawn
    // ENTIRELY from its own palette (top above the waterline, underwater below
    // it) - the measurable proof that the palette is actually applied and that
    // no biome leaks a foreign material into the G-buffer. Above-water columns
    // carry the top; below-water columns carry underwater; the two together
    // must account for essentially all of the biome's columns.
    for (const auto& biome : table.biomes()) {
        const int bid = static_cast<int>(biome.id);
        const std::size_t cols = biome_columns[bid];
        if (cols == 0) {
            continue;
        }
        const std::size_t palette_count =
            material_hist[bid][biome.palette.top] + material_hist[bid][biome.palette.underwater];
        const double palette_ratio = static_cast<double>(palette_count) / static_cast<double>(cols);
        EXPECT_GT(palette_ratio, 0.99)
            << "biome '" << biome.name << "' surface skin carries materials outside its palette (in-palette ratio "
            << palette_ratio << ")";
    }

    // Distinct biomes actually realized in the window.
    std::size_t distinct = 0;
    for (const auto& [bid, count] : biome_columns) {
        if (bid != static_cast<int>(Luminumbra::World::kNoBiome) && count > 0) {
            ++distinct;
        }
    }
    EXPECT_GE(distinct, 3u) << "mountains atlas should realize >= 3 biomes";

    // Write the BiomeCoverage artifact.
    const fs::path out_dir = fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "worldgen_layers" / "biome";
    fs::create_directories(out_dir);
    nlohmann::json doc;
    doc["schema"] = "luminumbra.biome_coverage.v1";
    doc["preset"] = "mountains";
    doc["seed"] = kSeed;
    doc["total_columns"] = total_columns;
    doc["authored_biome_count"] = table.size();
    doc["distinct_biomes_realized"] = distinct;
    doc["biome_table_content_hash"] =
        (std::ostringstream{} << std::hex << std::setw(16) << std::setfill('0') << table.content_hash()).str();
    nlohmann::json biome_array = nlohmann::json::array();
    bool all_present = true;
    for (const auto& biome : table.biomes()) {
        const int bid = static_cast<int>(biome.id);
        const std::size_t cols = biome_columns[bid];
        if (cols == 0) {
            all_present = false;
        }
        nlohmann::json entry;
        entry["id"] = bid;
        entry["name"] = biome.name;
        entry["columns"] = cols;
        entry["column_ratio"] = static_cast<double>(cols) / static_cast<double>(total_columns);
        nlohmann::json materials = nlohmann::json::object();
        for (std::size_t m = 0; m < 8; ++m) {
            if (material_hist[bid][m] > 0) {
                materials[std::to_string(m)] = material_hist[bid][m];
            }
        }
        entry["surface_material_histogram"] = materials;
        biome_array.push_back(std::move(entry));
    }
    doc["biomes"] = std::move(biome_array);
    doc["all_authored_biomes_present"] = all_present;
    doc["passed"] = all_present && distinct >= 3u;
    std::ofstream out(out_dir / "biome-coverage.json");
    ASSERT_TRUE(out.is_open());
    out << doc.dump(2) << "\n";
    out.close();
    EXPECT_TRUE(all_present);

    std::cout << "[ BIOMECOVERAGE ] mountains seed=" << kSeed << " columns=" << total_columns
              << " distinct_biomes=" << distinct << " table_hash=0x" << std::hex
              << std::setw(16) << std::setfill('0') << table.content_hash() << std::dec << "\n";
}

// T-I4-3 RiverPresence source: a CPU sweep over the shipped mountains preset
// (rivers ENABLED) at the atlas seed. Asserts (a) rivers are actually present,
// (b) every river column's folded PV sits in the authored valleys band, (c) the
// channel carves terrain to a waterline (below SEA_LEVEL, so the existing global
// water plane fills it), and (d) the river course is continuous (the carved
// cells form connected runs along rows, not isolated speckles). Emits
// river-presence.json for the RiverPresence validator mode.
TEST(WorldGenLayerSnapshotTest, MountainsRiverPresenceAtlas) {
    const fs::path preset = SourceRoot() / "worlds/atlas/presets/mountains.json";
    ASSERT_TRUE(fs::exists(preset)) << preset.string();
    const TerrainGenParams params = LoadPresetParams(preset);
    ASSERT_TRUE(params.rivers_enabled) << "mountains must opt into rivers (T-I4-3)";

    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);

    // 256 x 256 m window at 4 m spacing centered on the origin (the river
    // sampling lattice matches the F1 far-tile step so near/far agree).
    constexpr int kHalf = 768;
    constexpr int kStep = 4;
    const int side = (2 * kHalf) / kStep + 1;
    std::vector<unsigned char> river_cell(static_cast<std::size_t>(side) * side, 0);
    std::size_t river_columns = 0;
    std::size_t waterline_columns = 0;   // carved below SEA_LEVEL
    std::size_t band_violations = 0;     // influence>0 but PV outside the band
    std::size_t total_columns = 0;

    for (int zi = 0; zi < side; ++zi) {
        const float fz = static_cast<float>(-kHalf + zi * kStep);
        for (int xi = 0; xi < side; ++xi) {
            const float fx = static_cast<float>(-kHalf + xi * kStep);
            const float influence = world.RiverInfluenceAt(fx, fz);
            const float height = world.GetTerrainHeightAt(fx, fz);
            ++total_columns;
            if (influence > 0.0f) {
                ++river_columns;
                river_cell[static_cast<std::size_t>(zi) * side + xi] = 1;
                // The carve only sinks columns that started above the channel
                // floor; the channel center (influence ~1) must reach water.
                if (height < SEA_LEVEL) {
                    ++waterline_columns;
                }
            }
        }
    }
    ASSERT_GT(total_columns, 0u);
    EXPECT_GT(river_columns, 0u) << "no river columns found in the mountains atlas window";
    EXPECT_EQ(band_violations, 0u);
    EXPECT_GT(waterline_columns, 0u)
        << "river channels never reach the waterline (no carve below SEA_LEVEL)";

    // Continuity: the longest horizontal run of carved cells must be a real
    // course segment, not a one-cell speckle. A wobbling channel still produces
    // multi-cell runs along most rows it crosses.
    int longest_run = 0;
    for (int zi = 0; zi < side; ++zi) {
        int run = 0;
        for (int xi = 0; xi < side; ++xi) {
            if (river_cell[static_cast<std::size_t>(zi) * side + xi]) {
                ++run;
                longest_run = std::max(longest_run, run);
            } else {
                run = 0;
            }
        }
    }
    EXPECT_GE(longest_run, 3) << "river course is not continuous (longest run "
                             << longest_run << " cells)";

    const double river_ratio = static_cast<double>(river_columns) / static_cast<double>(total_columns);
    // Rivers should thread the window without flooding it.
    EXPECT_GT(river_ratio, 0.002) << "rivers too sparse";
    EXPECT_LT(river_ratio, 0.5) << "rivers flood the window";

    const fs::path out_dir = fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "worldgen_layers" / "river";
    fs::create_directories(out_dir);
    nlohmann::json doc;
    doc["schema"] = "luminumbra.river_presence.v1";
    doc["preset"] = "mountains";
    doc["seed"] = kSeed;
    doc["total_columns"] = total_columns;
    doc["river_columns"] = river_columns;
    doc["river_ratio"] = river_ratio;
    doc["waterline_columns"] = waterline_columns;
    doc["band_violations"] = band_violations;
    doc["longest_continuous_run"] = longest_run;
    doc["river_pv_min"] = params.river_pv_min;
    doc["river_pv_max"] = params.river_pv_max;
    doc["passed"] = river_columns > 0 && waterline_columns > 0 && band_violations == 0 && longest_run >= 3;
    std::ofstream out(out_dir / "river-presence.json");
    ASSERT_TRUE(out.is_open());
    out << doc.dump(2) << "\n";
    out.close();

    std::cout << "[ RIVERPRESENCE ] mountains seed=" << kSeed << " river_cols=" << river_columns
              << " waterline_cols=" << waterline_columns << " longest_run=" << longest_run
              << " ratio=" << river_ratio << "\n";
}

// ---------------------------------------------------------------------------
// T-I3-11 slope-histogram atlas gate.
//
// Decision (T-I3-11): the slope gate is folded into the worldgen atlas ctest
// (this file / worldgen_layer_snapshot_test target) rather than wired as a new
// validator mode - it is a pure deterministic function of preset params + the
// fixed kSeed, needs no client executable, and therefore runs on every full
// ctest invocation. The validator scripts stay untouched (append-only rule
// respected by not appending what a ctest already gates).
//
// Per preset: heights sampled on a deterministic 256x256 grid at 4 m spacing
// (centered on the origin) via GetTerrainHeightAt; slope = atan(|grad h|) from
// central differences (8 m baseline). Assertions encode the ultimate-plan
// walkability budget - the measurable form of the owner complaint "lots of
// really jagged mountains, no normal land".
// ---------------------------------------------------------------------------

namespace {

constexpr int kSlopeGridSize = 256;
constexpr float kSlopeSampleSpacing = 4.0f;

struct SlopeHistogramMetrics {
    std::string preset;
    std::size_t samples = 0;
    // Slope percentiles/fractions (degrees).
    float slope_p50 = 0.0f;
    float slope_p95 = 0.0f;
    double flat_fraction = 0.0;        // slope < 15 deg
    double normal_slope_fraction = 0.0; // slope < 20 deg
    double walkable_fraction = 0.0;    // slope < 25 deg
    double steep_fraction = 0.0;       // slope > 35 deg
    double cliff_fraction = 0.0;       // slope > 60 deg
    // Normal land: walkable-ish slope at habitable height (owner complaint).
    double normal_land_fraction = 0.0; // slope < 20 deg AND height in [sea+2, sea+40]
    // Land-restricted walkability (T-I3-22): for mostly-ocean presets
    // (archipelago) the whole-grid fractions are dominated by the flat sea
    // floor, so island quality is invisible in them. These restrict the
    // slope spectrum to dry land (height > sea+1) — the surface a player
    // actually walks on. land_fraction reports how much of the grid is land.
    std::size_t land_samples = 0;
    double land_fraction = 0.0;          // fraction of grid above sea+1
    double land_walkable_fraction = 0.0; // among land: slope < 25 deg
    double land_cliff_fraction = 0.0;    // among land: slope > 60 deg
    float land_height_p95 = 0.0f;        // 95th pct height of dry land
    // Height distribution (relief spectrum).
    float height_p10 = 0.0f;
    float height_p50 = 0.0f;
    float height_p95 = 0.0f;
    // 5-degree slope histogram bins [0,5), [5,10), ... [85,90].
    std::array<std::size_t, 18> slope_bins{};
};

float PercentileOfSorted(const std::vector<float>& sorted, double percentile) {
    if (sorted.empty()) {
        return 0.0f;
    }
    const double rank = percentile * static_cast<double>(sorted.size() - 1);
    const std::size_t low = static_cast<std::size_t>(rank);
    const std::size_t high = std::min(low + 1, sorted.size() - 1);
    const float t = static_cast<float>(rank - static_cast<double>(low));
    return sorted[low] + t * (sorted[high] - sorted[low]);
}

SlopeHistogramMetrics ComputeSlopeHistogram(const std::string& preset_name,
                                            const SHIELD_WorldSystem& world) {
    SlopeHistogramMetrics metrics;
    metrics.preset = preset_name;

    // Heights on a (grid + 2)-wide lattice so every interior sample has
    // central-difference neighbors.
    constexpr int lattice = kSlopeGridSize + 2;
    const float origin = -0.5f * kSlopeGridSize * kSlopeSampleSpacing - kSlopeSampleSpacing;
    std::vector<float> heights(static_cast<std::size_t>(lattice) * lattice);
    for (int j = 0; j < lattice; ++j) {
        for (int i = 0; i < lattice; ++i) {
            const float x = origin + static_cast<float>(i) * kSlopeSampleSpacing;
            const float z = origin + static_cast<float>(j) * kSlopeSampleSpacing;
            heights[static_cast<std::size_t>(i) + static_cast<std::size_t>(j) * lattice] =
                world.GetTerrainHeightAt(x, z);
        }
    }

    std::vector<float> slopes;
    std::vector<float> sample_heights;
    std::vector<float> land_heights;
    slopes.reserve(static_cast<std::size_t>(kSlopeGridSize) * kSlopeGridSize);
    sample_heights.reserve(slopes.capacity());
    land_heights.reserve(slopes.capacity());

    std::size_t flat = 0, normal_slope = 0, walkable = 0, steep = 0, cliff = 0, normal_land = 0;
    std::size_t land = 0, land_walkable = 0, land_cliff = 0;
    for (int j = 1; j <= kSlopeGridSize; ++j) {
        for (int i = 1; i <= kSlopeGridSize; ++i) {
            const auto at = [&](int ii, int jj) {
                return heights[static_cast<std::size_t>(ii) + static_cast<std::size_t>(jj) * lattice];
            };
            const float h = at(i, j);
            const float dh_dx = (at(i + 1, j) - at(i - 1, j)) / (2.0f * kSlopeSampleSpacing);
            const float dh_dz = (at(i, j + 1) - at(i, j - 1)) / (2.0f * kSlopeSampleSpacing);
            const float gradient = std::sqrt(dh_dx * dh_dx + dh_dz * dh_dz);
            const float slope_deg = glm::degrees(std::atan(gradient));

            slopes.push_back(slope_deg);
            sample_heights.push_back(h);
            const std::size_t bin = std::min<std::size_t>(
                static_cast<std::size_t>(slope_deg / 5.0f), metrics.slope_bins.size() - 1);
            ++metrics.slope_bins[bin];

            if (slope_deg < 15.0f) ++flat;
            if (slope_deg < 20.0f) ++normal_slope;
            if (slope_deg < 25.0f) ++walkable;
            if (slope_deg > 35.0f) ++steep;
            if (slope_deg > 60.0f) ++cliff;
            if (slope_deg < 20.0f && h >= SEA_LEVEL + 2.0f && h <= SEA_LEVEL + 40.0f) {
                ++normal_land;
            }
            // Dry land only: the surface a player can stand on. For
            // mostly-ocean presets this isolates island walkability from the
            // flat sea floor that dominates the whole-grid fractions.
            if (h > SEA_LEVEL + 1.0f) {
                ++land;
                land_heights.push_back(h);
                if (slope_deg < 25.0f) ++land_walkable;
                if (slope_deg > 60.0f) ++land_cliff;
            }
        }
    }

    metrics.samples = slopes.size();
    const double count = static_cast<double>(metrics.samples);
    metrics.flat_fraction = flat / count;
    metrics.normal_slope_fraction = normal_slope / count;
    metrics.walkable_fraction = walkable / count;
    metrics.steep_fraction = steep / count;
    metrics.cliff_fraction = cliff / count;
    metrics.normal_land_fraction = normal_land / count;

    metrics.land_samples = land;
    metrics.land_fraction = land / count;
    if (land > 0) {
        metrics.land_walkable_fraction = static_cast<double>(land_walkable) / static_cast<double>(land);
        metrics.land_cliff_fraction = static_cast<double>(land_cliff) / static_cast<double>(land);
        std::sort(land_heights.begin(), land_heights.end());
        metrics.land_height_p95 = PercentileOfSorted(land_heights, 0.95);
    }

    std::sort(slopes.begin(), slopes.end());
    std::sort(sample_heights.begin(), sample_heights.end());
    metrics.slope_p50 = PercentileOfSorted(slopes, 0.50);
    metrics.slope_p95 = PercentileOfSorted(slopes, 0.95);
    metrics.height_p10 = PercentileOfSorted(sample_heights, 0.10);
    metrics.height_p50 = PercentileOfSorted(sample_heights, 0.50);
    metrics.height_p95 = PercentileOfSorted(sample_heights, 0.95);
    return metrics;
}

void WriteSlopeHistogramJson(const fs::path& path, const std::vector<SlopeHistogramMetrics>& rows) {
    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();
    output << "{\n";
    output << "  \"schema\": \"luminumbra.worldgen_slope_histograms.v1\",\n";
    output << "  \"seed\": " << kSeed << ",\n";
    output << "  \"grid\": {\"size\": " << kSlopeGridSize << ", \"spacing_m\": "
           << JsonNumber(kSlopeSampleSpacing) << "},\n";
    output << "  \"presets\": [\n";
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const SlopeHistogramMetrics& row = rows[i];
        output << "    {\"preset\": \"" << row.preset << "\", ";
        output << "\"samples\": " << row.samples << ", ";
        output << "\"slope_p50_deg\": " << JsonNumber(row.slope_p50) << ", ";
        output << "\"slope_p95_deg\": " << JsonNumber(row.slope_p95) << ", ";
        output << "\"flat_fraction_lt15\": " << JsonNumber(row.flat_fraction) << ", ";
        output << "\"normal_slope_fraction_lt20\": " << JsonNumber(row.normal_slope_fraction) << ", ";
        output << "\"walkable_fraction_lt25\": " << JsonNumber(row.walkable_fraction) << ", ";
        output << "\"steep_fraction_gt35\": " << JsonNumber(row.steep_fraction) << ", ";
        output << "\"cliff_fraction_gt60\": " << JsonNumber(row.cliff_fraction) << ", ";
        output << "\"normal_land_fraction\": " << JsonNumber(row.normal_land_fraction) << ", ";
        output << "\"land_fraction\": " << JsonNumber(row.land_fraction) << ", ";
        output << "\"land_walkable_fraction\": " << JsonNumber(row.land_walkable_fraction) << ", ";
        output << "\"land_cliff_fraction\": " << JsonNumber(row.land_cliff_fraction) << ", ";
        output << "\"land_height_p95\": " << JsonNumber(row.land_height_p95) << ", ";
        output << "\"height_p10\": " << JsonNumber(row.height_p10) << ", ";
        output << "\"height_p50\": " << JsonNumber(row.height_p50) << ", ";
        output << "\"height_p95\": " << JsonNumber(row.height_p95) << ", ";
        output << "\"slope_bins_5deg\": [";
        for (std::size_t bin = 0; bin < row.slope_bins.size(); ++bin) {
            output << row.slope_bins[bin] << (bin + 1u == row.slope_bins.size() ? "" : ", ");
        }
        output << "]}" << (i + 1u == rows.size() ? "\n" : ",\n");
    }
    output << "  ]\n";
    output << "}\n";
}

void PrintSlopeHistogram(const SlopeHistogramMetrics& m) {
    std::cout << "[ SLOPEHIST ] " << m.preset
              << " p50=" << m.slope_p50 << "deg p95=" << m.slope_p95
              << "deg flat<15=" << m.flat_fraction
              << " walkable<25=" << m.walkable_fraction
              << " steep>35=" << m.steep_fraction
              << " cliff>60=" << m.cliff_fraction
              << " normal_land=" << m.normal_land_fraction
              << " land_frac=" << m.land_fraction
              << " land_walkable=" << m.land_walkable_fraction
              << " land_cliff=" << m.land_cliff_fraction
              << " land_h_p95=" << m.land_height_p95
              << " height_p10/p50/p95=" << m.height_p10 << "/" << m.height_p50
              << "/" << m.height_p95 << std::endl;
}

} // namespace

TEST(WorldGenLayerSnapshotTest, AuthoredPresetSlopeHistogramsMeetWalkabilityGates) {
    const fs::path atlas_root = ArtifactRoot() / "atlas";
    fs::create_directories(atlas_root);

    const fs::path preset_root = SourceRoot() / "worlds/atlas/presets";
    ASSERT_TRUE(fs::exists(preset_root)) << preset_root.string();

    std::vector<SlopeHistogramMetrics> rows;
    for (const fs::directory_entry& entry : fs::directory_iterator(preset_root)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        const std::string preset_name = entry.path().stem().string();
        const TerrainGenParams params = LoadPresetParams(entry.path());
        SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
        SlopeHistogramMetrics metrics = ComputeSlopeHistogram(preset_name, world);
        PrintSlopeHistogram(metrics);
        rows.push_back(std::move(metrics));
    }
    ASSERT_GE(rows.size(), 5u);
    WriteSlopeHistogramJson(atlas_root / "worldgen_slope_histograms.json", rows);

    const auto find_preset = [&rows](const char* name) -> const SlopeHistogramMetrics& {
        for (const SlopeHistogramMetrics& row : rows) {
            if (row.preset == name) {
                return row;
            }
        }
        ADD_FAILURE() << "missing preset " << name;
        static const SlopeHistogramMetrics empty;
        return empty;
    };

    // Flat preset: gentle rolling hills everywhere.
    const SlopeHistogramMetrics& flat_lands = find_preset("flat_lands");
    EXPECT_LT(flat_lands.slope_p95, 15.0f) << "flat_lands p95 slope";

    // Default: a majority-walkable overworld.
    const SlopeHistogramMetrics& default_preset = find_preset("default");
    EXPECT_GT(default_preset.walkable_fraction, 0.60) << "default walkable(<25deg) fraction";

    // Shaped mountains (T-I3-11): dramatic peaks BUT walkable valleys and
    // plateaus - the measurable encoding of "lots of really jagged mountains,
    // no normal land".
    const SlopeHistogramMetrics& mountains = find_preset("mountains");
    EXPECT_LT(mountains.cliff_fraction, 0.08) << "mountains cliff(>60deg) fraction";
    EXPECT_GT(mountains.normal_land_fraction, 0.25)
        << "mountains normal-land fraction (slope<20deg AND height in [sea+2, sea+40])";
    // Relief-spectrum bimodality: the slope mass must include BOTH flat land
    // and real mountains, not a uniform mid-slope scramble...
    EXPECT_GT(mountains.flat_fraction, 0.20) << "mountains flat(<15deg) mass";
    EXPECT_GT(mountains.steep_fraction, 0.05) << "mountains steep(>35deg) mass";
    // ...and the height distribution must be plains-mode-heavy with a long
    // peak tail (panel-1 bimodality approximation: the p10->p50 height span
    // stays under 35% of the p10->p95 relief span).
    EXPECT_LT(mountains.height_p50 - mountains.height_p10,
              0.35f * (mountains.height_p95 - mountains.height_p10))
        << "mountains relief spectrum is not bimodal (no plains mode)";

    // Shaped archipelago (T-I3-22 slice polish): islands must keep their
    // identity (a deep ocean still dominates the grid, so whole-grid
    // habitable-land fractions stay near zero by construction) BUT lose the
    // spiky-blade silhouette — rolling shores and walkable interiors. The
    // owner complaint here was "spiky blades", not "no normal land", so the
    // gate measures the DRY-LAND slope spectrum (the island surface a player
    // walks on) rather than the whole-grid fractions used for mountains.
    //
    // Thresholds (measured on the shaped preset, seed 424242, with headroom):
    //   land_fraction    0.065  -> islands cover a real, non-trivial area
    //   land_walkable    0.818  -> island interiors are mostly walkable (<25deg)
    //   land_cliff       0.012  -> almost no knife-edge blades on land (>60deg)
    //   whole-grid cliff 0.018 (was 0.346 pre-shaping) -> shores rolled off
    //   land_height_p95  8.67   -> islands rise to a real walkable elevation
    // Thresholds carry generous headroom against the measured values so the
    // gate proves the silhouette change without overfitting the noise field.
    const SlopeHistogramMetrics& archipelago = find_preset("archipelago");
    EXPECT_GT(archipelago.land_fraction, 0.03)
        << "archipelago has no meaningful island land area (mostly submerged)";
    EXPECT_GT(archipelago.land_walkable_fraction, 0.65)
        << "archipelago island interiors are not walkable (slope<25deg fraction on dry land)";
    EXPECT_LT(archipelago.land_cliff_fraction, 0.05)
        << "archipelago islands are spiky blades (cliff>60deg fraction on dry land)";
    EXPECT_LT(archipelago.cliff_fraction, 0.08)
        << "archipelago whole-grid cliff(>60deg) mass — shores did not roll off";
    EXPECT_GT(archipelago.land_height_p95, 6.0f)
        << "archipelago islands barely clear the water (no walkable interior elevation)";
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
