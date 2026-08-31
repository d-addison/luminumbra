// Generates the small authored test textures committed under
// data/textures/test/ used by the.ltex round-trip test.
//
// These are deterministic procedural patterns (no external source art). Built
// and run once to (re)produce the committed PNGs; it is NOT part of the CMake
// build. To regenerate:
//   g++ -std=c++17 -I vendor/stb tools/generate_test_textures.cpp -o gen_tex
//./gen_tex
//
// Patterns are chosen to exercise distinct mip behaviour: a checkerboard (high
// frequency -> mips converge to grey), a smooth gradient, and a non-square
// texture (exercises independent width/height halving and floor-to-1).

#include <cstdint>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace {

void WritePng(const std::string& path, int w, int h, const std::vector<uint8_t>& rgba) {
    stbi_write_png(path.c_str(), w, h, 4, rgba.data(), w * 4);
}

// 16x16 RGB checkerboard with 2x2 cells.
std::vector<uint8_t> Checkerboard(int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool on = (((x / 2) + (y / 2)) & 1) != 0;
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            const uint8_t v = on ? 230 : 20;
            px[i + 0] = v;
            px[i + 1] = static_cast<uint8_t>(on ? 40 : 200);
            px[i + 2] = static_cast<uint8_t>(on ? 120 : 90);
            px[i + 3] = 255;
        }
    }
    return px;
}

// 16x16 smooth diagonal gradient.
std::vector<uint8_t> Gradient(int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            px[i + 0] = static_cast<uint8_t>((x * 255) / (w - 1));
            px[i + 1] = static_cast<uint8_t>((y * 255) / (h - 1));
            px[i + 2] = static_cast<uint8_t>(((x + y) * 255) / (w + h - 2));
            px[i + 3] = 255;
        }
    }
    return px;
}

// 32x8 non-square framed pattern (border + center), exercises independent
// width/height mip halving down to 1x1.
std::vector<uint8_t> Framed(int w, int h) {
    std::vector<uint8_t> px(static_cast<size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            const bool border = (x == 0 || y == 0 || x == w - 1 || y == h - 1);
            px[i + 0] = static_cast<uint8_t>(border ? 250 : 60);
            px[i + 1] = static_cast<uint8_t>(border ? 250 : 140);
            px[i + 2] = static_cast<uint8_t>(border ? 30 : 210);
            px[i + 3] = 255;
        }
    }
    return px;
}

} // namespace

int main() {
    const std::string dir = "data/textures/test/";
    WritePng(dir + "checker_16.png", 16, 16, Checkerboard(16, 16));
    WritePng(dir + "gradient_16.png", 16, 16, Gradient(16, 16));
    WritePng(dir + "framed_32x8.png", 32, 8, Framed(32, 8));
    return 0;
}
