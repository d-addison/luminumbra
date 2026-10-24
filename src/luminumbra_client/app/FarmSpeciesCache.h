#pragma once

#include "luminumbra_common/foliage/SpeciesRegistry.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace Luminumbra::Client::App {

// Interactive metadata only. The caller supplies its existing monotonic frame
// time in seconds; waiting frames perform no filesystem work or diagnostic copies.
class FarmSpeciesCache {
public:
    enum class UpdateResult : std::uint8_t {
        Unchanged,
        Diagnostics,
        Recovered
    };

    [[nodiscard]] bool NeedsUpdate(double frame_seconds) const {
        return !loaded_ && (!failed_ || frame_seconds >= next_retry_seconds_);
    }

    UpdateResult Update(double frame_seconds, const std::filesystem::path& directory) {
        if (!NeedsUpdate(frame_seconds))
            return UpdateResult::Unchanged;

        std::vector<std::string> errors;
        bool io_failed = false;
        registry_.LoadFromDirectory(directory, errors, &io_failed);
        if (io_failed) {
            next_retry_seconds_ = frame_seconds + kRetrySeconds;
            const bool changed = !failed_ || errors != diagnostics_;
            failed_ = true;
            diagnostics_ = std::move(errors);
            return changed ? UpdateResult::Diagnostics : UpdateResult::Unchanged;
        }

        loaded_ = true;
        diagnostics_ = std::move(errors);
        return failed_ ? UpdateResult::Recovered : UpdateResult::Diagnostics;
    }

    [[nodiscard]] bool loaded() const {
        return loaded_;
    }
    [[nodiscard]] const std::vector<luminumbra::foliage::SpeciesTemplate>& all() const {
        return registry_.all();
    }
    // Diagnostics for the most recent attempt; log only on a reported transition.
    [[nodiscard]] const std::vector<std::string>& diagnostics() const {
        return diagnostics_;
    }

private:
    static constexpr double kRetrySeconds = 5.0;
    luminumbra::foliage::SpeciesRegistry registry_;
    std::vector<std::string> diagnostics_;
    double next_retry_seconds_ = 0.0;
    bool loaded_ = false;
    bool failed_ = false;
};

} // namespace Luminumbra::Client::App
