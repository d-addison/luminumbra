// Generates the authored grovestrider creature textures committed under
// data/textures/creatures/grovestrider/ ( skinned texturing).
//
// Deterministic procedural patterns (no external source art). Built and run
// once to (re)produce the committed PNGs; NOT part of the CMake build. To
// regenerate:
//   g++ -std=c++17 -I vendor/stb tools/generate_creature_textures.cpp -o gen_creature
//./gen_creature
//   build/.../asset_processor grovestrider_albedo.png grovestrider_albedo_256.ltex 256
//   --preview-png build/.../asset_processor grovestrider_normal.png grovestrider_normal_256.ltex
//   256 --preview-png
//
// The grovestrider's box UVs map each face to [0,1]^2, so the albedo carries
// a recognisable creature read: a mossy green body gradient, darker dorsal
// banding, warm under-belly, and bright eye/marking spots. The variation is
// deliberate and high-contrast so the skinned-mesh visual gate's color-variance
// check has a clear signal (a flat color would fail it).

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

void WritePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4);
}

uint8_t clamp8(float v) {
    if (v < 0.0f)
        v = 0.0f;
    if (v > 255.0f)
        v = 255.0f;
    return static_cast<uint8_t>(v + 0.5f);
}

// Cheap value-noise hash for surface speckle.
float hash(int x, int y) {
    uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xffff) / 65535.0f;
}

std::vector<uint8_t> Albedo(int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float u = static_cast<float>(x) / (w - 1);
            const float v = static_cast<float>(y) / (h - 1);
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;

            // Base mossy green body, lighter toward the belly (high v).
            float r = 46.0f + 60.0f * v;
            float g = 96.0f + 70.0f * v;
            float b = 40.0f + 30.0f * v;

            // Dorsal banding (dark stripes across the back, low v).
            float band = std::sin(u * 18.84955592f); // ~3 stripes
            if (v < 0.5f && band > 0.4f) {
                r *= 0.55f;
                g *= 0.55f;
                b *= 0.55f;
            }

            // Warm under-belly tint.
            if (v > 0.72f) {
                r += 50.0f;
                g += 18.0f;
                b += 8.0f;
            }

            // Bright eye/marking spots near the head band (u in [0.1,0.2]).
            const float du = u - 0.15f;
            const float dv = v - 0.30f;
            if (du * du + dv * dv < 0.0035f) {
                r = 235.0f;
                g = 220.0f;
                b = 70.0f;
            }

            // Fine speckle.
            const float n = (hash(x, y) - 0.5f) * 22.0f;
            r += n;
            g += n;
            b += n;

            px[i + 0] = clamp8(r);
            px[i + 1] = clamp8(g);
            px[i + 2] = clamp8(b);
            px[i + 3] = 255;
        }
    }
    return px;
}

std::vector<uint8_t> Normal(int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const float u = static_cast<float>(x) / (w - 1);
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            // Tangent-space (OpenGL) normal: ridged dorsal banding perturbs the
            // x slope; everything else is near-flat (z up).
            float nx = 0.30f * std::cos(u * 18.84955592f);
            float ny = (hash(x, y) - 0.5f) * 0.18f;
            float nz = std::sqrt(std::max(0.0f, 1.0f - nx * nx - ny * ny));
            px[i + 0] = clamp8((nx * 0.5f + 0.5f) * 255.0f);
            px[i + 1] = clamp8((ny * 0.5f + 0.5f) * 255.0f);
            px[i + 2] = clamp8((nz * 0.5f + 0.5f) * 255.0f);
            px[i + 3] = 255;
        }
    }
    return px;
}

} // namespace

int main() {
    const std::string dir = "data/textures/creatures/grovestrider/";
    WritePng(dir + "grovestrider_albedo.png", 256, 256, Albedo(256, 256));
    WritePng(dir + "grovestrider_normal.png", 256, 256, Normal(256, 256));
    return 0;
}
