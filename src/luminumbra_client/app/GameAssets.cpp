#include "GameAssets.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace Luminumbra::Client::App {
namespace {

constexpr std::array<std::uint32_t, 64> kRoundConstants = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void Compress(std::array<std::uint32_t, 8>& state, const std::array<unsigned char, 64>& block) {
    std::array<std::uint32_t, 64> words{};
    for (size_t i = 0; i < 16; ++i)
        for (size_t byte = 0; byte < 4; ++byte)
            words[i] = (words[i] << 8) | block[i * 4 + byte];
    for (size_t i = 16; i < words.size(); ++i) {
        const auto x = words[i - 15], y = words[i - 2];
        words[i] = words[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3)) + words[i - 7] +
                   (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
    }
    auto work = state;
    for (size_t i = 0; i < words.size(); ++i) {
        const auto a = work[0], b = work[1], c = work[2], e = work[4], f = work[5], g = work[6];
        const auto first = work[7] + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) +
                           ((e & f) ^ (~e & g)) + kRoundConstants[i] + words[i];
        const auto second =
            (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        for (size_t j = 7; j > 0; --j)
            work[j] = work[j - 1];
        work[4] += first;
        work[0] = first + second;
    }
    for (size_t i = 0; i < state.size(); ++i)
        state[i] += work[i];
}

std::filesystem::path CheckedPath(const std::filesystem::path& root, const std::string& relative) {
    const std::filesystem::path path(relative);
    if (relative.empty() || path.is_absolute() || path.generic_string() != relative ||
        relative.find_first_of("\\:") != std::string::npos ||
        relative.find('\0') != std::string::npos)
        throw std::runtime_error("Invalid path in game asset manifest");
    auto result = root;
    for (const auto& part : path) {
        if (part.empty() || part == "." || part == "..")
            throw std::runtime_error("Invalid path in game asset manifest");
        result /= part;
        bool redirected = std::filesystem::is_symlink(std::filesystem::symlink_status(result));
#ifdef _WIN32
        // MSVC reports NTFS junctions as file_type::junction, not as symlinks.
        const auto attributes = GetFileAttributesW(result.c_str());
        redirected = redirected || (attributes != INVALID_FILE_ATTRIBUTES &&
                                    (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
#endif
        if (redirected)
            throw std::runtime_error(
                "Game asset path traverses a symbolic link or reparse point: " + result.string());
    }
    return result;
}

} // namespace

std::string AssetFileSha256(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Could not read required asset: " + path.string());
    std::array<std::uint32_t, 8> state = {0x6a09e667,
                                          0xbb67ae85,
                                          0x3c6ef372,
                                          0xa54ff53a,
                                          0x510e527f,
                                          0x9b05688c,
                                          0x1f83d9ab,
                                          0x5be0cd19};
    std::uint64_t bytes = 0;
    std::array<unsigned char, 64> block{};
    for (;;) {
        input.read(reinterpret_cast<char*>(block.data()), block.size());
        const auto count = static_cast<size_t>(input.gcount());
        bytes += count;
        if (bytes > 512u * 1024u * 1024u || input.bad())
            throw std::runtime_error("Required asset is unreadable or exceeds 512 MiB: " +
                                     path.string());
        if (count == block.size()) {
            Compress(state, block);
            continue;
        }
        std::fill(block.begin() + static_cast<std::ptrdiff_t>(count), block.end(), 0);
        block[count] = 0x80;
        if (count >= 56) {
            Compress(state, block);
            block.fill(0);
        }
        const auto bits = bytes * 8;
        for (size_t i = 0; i < 8; ++i)
            block[63 - i] = static_cast<unsigned char>(bits >> (i * 8));
        Compress(state, block);
        break;
    }
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto word : state)
        output << std::setw(8) << word;
    return output.str();
}

std::string VerifyGameAssets(const std::filesystem::path& root) {
    try {
        const auto manifestPath = root / "config/game-asset-packs.json";
        if (std::filesystem::file_size(manifestPath) > 1024u * 1024u)
            throw std::runtime_error("Game asset manifest exceeds 1 MiB");
        std::ifstream stream(manifestPath);
        const auto manifest = nlohmann::json::parse(stream);
        const auto& pack = manifest.at("packs").at("tree-small-02-runtime");
        if (manifest.at("schema") != "luminumbra.game.asset-packs.v1" ||
            pack.at("version") != "1.0.0" || pack.at("install_dir") != kTreePackDirectory)
            throw std::runtime_error("Unsupported game asset pack version");
        const auto directory = CheckedPath(root, kTreePackDirectory);
        const auto& files = pack.at("files");
        if (!files.is_array() || files.size() < 18 || files.size() > 32)
            throw std::runtime_error("Game asset manifest has an invalid required-file count");
        std::set<std::string> verified;
        for (const auto& item : files) {
            const auto relative = item.at("path").get<std::string>();
            const auto path = CheckedPath(directory, relative);
            const auto size = item.at("size").get<std::uint64_t>();
            const auto sha = item.at("sha256").get<std::string>();
            if (size == 0 || size > 64u * 1024u * 1024u || sha.size() != 64 ||
                !verified.insert(relative).second || !std::filesystem::is_regular_file(path) ||
                std::filesystem::file_size(path) != size || AssetFileSha256(path) != sha)
                throw std::runtime_error("Missing or corrupt required asset: " + path.string());
        }
        for (const std::string part : {"trunk", "branches", "leaves"})
            for (const std::string lod : {"", ".lod1", ".lod2"})
                if (!verified.contains("data/models/trees/tree_small_02_" + part + lod + ".lmesh"))
                    throw std::runtime_error("Game asset manifest omits a required tree mesh LOD");
        for (const std::string part : {"trunk", "branch", "leaves"})
            for (const std::string map : {"albedo", "normal", "arm"})
                if (!verified.contains("data/textures/models/tree_" + part + "_" + map +
                                       "_512.ltex"))
                    throw std::runtime_error("Game asset manifest omits a required tree material");
        return {};
    } catch (const std::exception& error) {
        return std::string("Required game content is unavailable. ") + error.what() +
               ". Run python3 tools/assets/acquire.py --repair from the game folder, then restart "
               "the client.";
    }
}

} // namespace Luminumbra::Client::App
