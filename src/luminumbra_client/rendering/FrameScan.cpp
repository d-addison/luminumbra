#include "FrameScan.h"

#include "RenderPipeline.h"
#include "core/Log.h"
#include "passes/FoliagePass.h"

#include <glad/glad.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <system_error>
#include <unordered_map>

namespace Luminumbra::Rendering {

namespace {

// Rec. 709 luminance of an sRGB byte triple, normalized to [0,1]. The back
// buffer is the final tonemapped sRGB image, so this is "how bright did this
// pixel render" — exactly what we want to summarize per material.
inline double LumaOf(unsigned char r, unsigned char g, unsigned char b) {
    return (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
}

// Best-effort id->name table from data/common/materials.json. Missing file ->
// empty map (the report then carries numeric ids only). Never throws out.
std::unordered_map<int, std::string> LoadMaterialNames(const std::filesystem::path& path) {
    std::unordered_map<int, std::string> names;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
        return names;
    try {
        std::ifstream f(path);
        nlohmann::json j;
        f >> j;
        if (j.contains("materials")) {
            for (const auto& m : j["materials"]) {
                if (m.contains("id") && m.contains("name")) {
                    names[m["id"].get<int>()] = m["name"].get<std::string>();
                }
            }
        }
    } catch (const std::exception& e) {
        LUMINUMBRA_CORE_WARN("FrameScan: materials.json parse failed ({}); ids only", e.what());
    }
    return names;
}

} // namespace

FrameScanReport ScanFrame(const RenderPipeline& pipeline,
                          int framebuffer_width,
                          int framebuffer_height,
                          const std::filesystem::path& materials_json) {
    FrameScanReport report;
    if (framebuffer_width <= 0 || framebuffer_height <= 0) {
        LUMINUMBRA_CORE_ERROR(
            "FrameScan: invalid framebuffer size {}x{}", framebuffer_width, framebuffer_height);
        return report;
    }
    report.width = framebuffer_width;
    report.height = framebuffer_height;
    const std::size_t pixel_count =
        static_cast<std::size_t>(framebuffer_width) * static_cast<std::size_t>(framebuffer_height);
    report.total_pixels = pixel_count;

    // --- 1. Read the back color buffer (final tonemapped sRGB) ----------------
    // The caller renders the frame and reads BEFORE any UI overlay, mirroring the
    // --scene-config / --timelapse capture sites.
    std::vector<unsigned char> color(pixel_count * 3u);
    glReadBuffer(GL_BACK);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(
        0, 0, framebuffer_width, framebuffer_height, GL_RGB, GL_UNSIGNED_BYTE, color.data());

    // --- 2. Read the G-buffer normal/material attachment (RGBA8) --------------
    // Alpha byte == MaterialID (g_buffer.frag writes alpha = MaterialID/255, so
    // the 8-bit byte recovers the id exactly). The texture is the full internal
    // render resolution; if that differs from the back-buffer framebuffer size
    // (e.g. an offscreen preview), we fall back to id-less reporting rather than
    // index past the smaller buffer.  readback (glGetTexImage).
    const GBuffer& gb = pipeline.gbuffer();
    std::vector<unsigned char> id_rgba;
    bool have_ids = false;
    if (gb.normal_texture != 0 && pipeline.screen_width() == static_cast<u32>(framebuffer_width) &&
        pipeline.screen_height() == static_cast<u32>(framebuffer_height)) {
        id_rgba.resize(pixel_count * 4u);
        glBindTexture(GL_TEXTURE_2D, gb.normal_texture);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, id_rgba.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        have_ids = true;
    } else if (gb.normal_texture != 0) {
        LUMINUMBRA_CORE_WARN(
            "FrameScan: G-buffer ({}x{}) != back buffer ({}x{}); reporting back-buffer luma only",
            pipeline.screen_width(),
            pipeline.screen_height(),
            framebuffer_width,
            framebuffer_height);
    }

    // --- 3. Accumulate per-material pixel count + summed luminance ------------
    // 256 possible 8-bit ids. Material 0 (Air) means "no opaque geometry wrote
    // here" — it is the sky / discarded-water passthrough and is NOT reported as
    // a material (but feeds the water heuristic below).
    std::array<std::uint64_t, 256> mat_pixels{};
    std::array<double, 256> mat_luma_sum{};
    std::array<double, 256> mat_luma_max{};
    double frame_luma_sum = 0.0;
    std::uint64_t water_pixels = 0;
    double water_luma_sum = 0.0;

    for (std::size_t p = 0; p < pixel_count; ++p) {
        const unsigned char r = color[p * 3u + 0u];
        const unsigned char g = color[p * 3u + 1u];
        const unsigned char b = color[p * 3u + 2u];
        const double luma = LumaOf(r, g, b);
        frame_luma_sum += luma;

        int id = -1;
        if (have_ids) {
            id = static_cast<int>(id_rgba[p * 4u + 3u]); // alpha byte == MaterialID
            mat_pixels[static_cast<std::size_t>(id)]++;
            mat_luma_sum[static_cast<std::size_t>(id)] += luma;
            double& mx = mat_luma_max[static_cast<std::size_t>(id)];
            if (luma > mx)
                mx = luma;
        }

        // Water (backbuffer heuristic): live water (id 7) is DISCARDED in the
        // G-buffer, so a water surface shows through as id 0 (Air) with the
        // forward water pass having painted a blue-dominant pixel into the back
        // buffer. Count those, plus any far-water sheet (id 200) which IS written
        // to the G-buffer. Blue-dominant == b meaningfully greater than r so warm
        // sky/terrain is not misread as water.
        // Blue-dominant test for water seen through the gbuffer. Relaxed to catch DARK water
        // (deep lakes/dusk water render quite dark) — was (b>r+12 && b>40) which read those as 0%.
        const bool blue_dominant = (b >= r + 6) && (b > 22) && (b >= g - 4);
        const bool gbuffer_open = !have_ids || id == 0; // nothing opaque written here
        const bool far_water = have_ids && id == 200;
        if (far_water || (gbuffer_open && blue_dominant)) {
            ++water_pixels;
            water_luma_sum += luma;
        }
    }

    report.mean_frame_luminance =
        pixel_count ? frame_luma_sum / static_cast<double>(pixel_count) : 0.0;
    report.water_pixels = water_pixels;
    report.water_coverage =
        pixel_count ? static_cast<double>(water_pixels) / static_cast<double>(pixel_count) : 0.0;
    report.water_mean_luminance =
        water_pixels ? water_luma_sum / static_cast<double>(water_pixels) : 0.0;

    // --- 4. Build the per-material list (skip Air id 0) -----------------------
    if (have_ids) {
        const auto names = LoadMaterialNames(materials_json);
        for (int id = 1; id < 256; ++id) {
            const std::uint64_t px = mat_pixels[static_cast<std::size_t>(id)];
            if (px == 0)
                continue;
            MaterialFrameStat stat;
            stat.id = id;
            if (auto it = names.find(id); it != names.end())
                stat.name = it->second;
            stat.pixels = px;
            stat.coverage = static_cast<double>(px) / static_cast<double>(pixel_count);
            stat.mean_luminance =
                mat_luma_sum[static_cast<std::size_t>(id)] / static_cast<double>(px);
            stat.max_luminance = mat_luma_max[static_cast<std::size_t>(id)];
            report.materials.push_back(std::move(stat));
        }
        std::sort(report.materials.begin(),
                  report.materials.end(),
                  [](const MaterialFrameStat& a, const MaterialFrameStat& b) {
                      return a.coverage > b.coverage;
                  });
    }

    // --- 5. Foliage tally (separate alpha-keyed scatter pass) -----------------
    if (const FoliagePass* fol = pipeline.foliage()) {
        report.foliage_instances = static_cast<std::uint64_t>(fol->frame_instance_count());
    }

    report.ok = true;
    return report;
}

bool WriteFrameScanReport(const FrameScanReport& report, const std::filesystem::path& out_path) {
    nlohmann::json j;
    j["schema"] = "luminumbra.frame_scan.v1";
    j["width"] = report.width;
    j["height"] = report.height;
    j["total_pixels"] = report.total_pixels;
    j["mean_frame_luminance"] = report.mean_frame_luminance;

    nlohmann::json mats = nlohmann::json::array();
    for (const auto& m : report.materials) {
        nlohmann::json mj;
        mj["id"] = m.id;
        mj["name"] = m.name;
        mj["pixels"] = m.pixels;
        mj["coverage"] = m.coverage;
        mj["mean_luminance"] = m.mean_luminance;
        mj["max_luminance"] = m.max_luminance;
        mats.push_back(std::move(mj));
    }
    j["materials"] = std::move(mats);

    nlohmann::json water;
    water["pixels"] = report.water_pixels;
    water["coverage"] = report.water_coverage;
    water["mean_luminance"] = report.water_mean_luminance;
    j["water"] = std::move(water);

    nlohmann::json foliage;
    foliage["instances"] = report.foliage_instances;
    j["foliage"] = std::move(foliage);

    std::error_code ec;
    if (out_path.has_parent_path())
        std::filesystem::create_directories(out_path.parent_path(), ec);
    std::ofstream out(out_path);
    if (!out) {
        LUMINUMBRA_CORE_ERROR("FrameScan: cannot write report -> {}", out_path.string());
        return false;
    }
    out << j.dump(2) << "\n";
    return true;
}

} // namespace Luminumbra::Rendering
