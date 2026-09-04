#pragma once

#include <nlohmann/json.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace luminumbra::test {

class SyntheticBankFixture {
public:
    SyntheticBankFixture(const std::filesystem::path& source_root,
                         const std::vector<std::filesystem::path>& banks)
        : root_(CreateUniqueRoot()) {
        try {
            for (const std::filesystem::path& bank : banks) {
                CopyManifestAndCreatePayloads(source_root, bank);
            }
        } catch (...) {
            std::error_code error;
            std::filesystem::remove_all(root_, error);
            throw;
        }
    }

    ~SyntheticBankFixture() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    SyntheticBankFixture(const SyntheticBankFixture&) = delete;
    SyntheticBankFixture& operator=(const SyntheticBankFixture&) = delete;

    [[nodiscard]] const std::filesystem::path& root() const {
        return root_;
    }

private:
    static std::filesystem::path CreateUniqueRoot() {
        static std::atomic<std::uint64_t> sequence = 0;
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const std::filesystem::path temp_root = std::filesystem::temp_directory_path();

        for (int attempt = 0; attempt < 128; ++attempt) {
            const std::filesystem::path candidate =
                temp_root / ("luminumbra-synthetic-audio-bank-" + std::to_string(timestamp) + "-" +
                             std::to_string(sequence.fetch_add(1)));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                return candidate;
            }
            if (error) {
                throw std::runtime_error("cannot create synthetic audio fixture: " +
                                         error.message());
            }
        }

        throw std::runtime_error("cannot allocate a unique synthetic audio fixture directory");
    }

    static void ValidateRelativePath(const std::filesystem::path& path) {
        if (path.empty() || path.is_absolute()) {
            throw std::runtime_error("audio-bank path must be relative: " + path.string());
        }
        for (const auto& component : path) {
            if (component == "..") {
                throw std::runtime_error("audio-bank path escapes fixture root: " + path.string());
            }
        }
    }

    static void WriteSilentWav(const std::filesystem::path& path) {
        // PCM, mono, 8 kHz, 16-bit, with one silent sample.
        static constexpr std::array<unsigned char, 46> kSilentWav = {
            'R', 'I', 'F', 'F', 38,  0,   0,   0,   'W',  'A',  'V', 'E', 'f',  'm',  't', ' ',
            16,  0,   0,   0,   1,   0,   1,   0,   0x40, 0x1f, 0,   0,   0x80, 0x3e, 0,   0,
            2,   0,   16,  0,   'd', 'a', 't', 'a', 2,    0,    0,   0,   0,    0,
        };

        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary);
        if (!output) {
            throw std::runtime_error("cannot create synthetic audio payload: " + path.string());
        }
        output.write(reinterpret_cast<const char*>(kSilentWav.data()),
                     static_cast<std::streamsize>(kSilentWav.size()));
        if (!output) {
            throw std::runtime_error("cannot write synthetic audio payload: " + path.string());
        }
    }

    void CopyManifestAndCreatePayloads(const std::filesystem::path& source_root,
                                       const std::filesystem::path& bank) const {
        ValidateRelativePath(bank);
        const std::filesystem::path source_manifest = source_root / bank;
        const std::filesystem::path fixture_manifest = root_ / bank;
        std::filesystem::create_directories(fixture_manifest.parent_path());
        std::filesystem::copy_file(source_manifest, fixture_manifest);

        std::ifstream input(fixture_manifest);
        if (!input) {
            throw std::runtime_error("cannot open copied audio manifest: " +
                                     fixture_manifest.string());
        }
        nlohmann::json manifest;
        input >> manifest;
        for (const auto& [id, definition] : manifest.at("events").items()) {
            static_cast<void>(id);
            for (const auto& file : definition.at("files")) {
                const std::filesystem::path relative_asset = file.get<std::string>();
                ValidateRelativePath(relative_asset);
                WriteSilentWav(root_ / relative_asset);
            }
        }
    }

    std::filesystem::path root_;
};

} // namespace luminumbra::test
