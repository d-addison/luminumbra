#pragma once

#include "world/ActiveRegionLedger.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace luminumbra::simulation {

// Budget profiles are operator-supplied limits. They never affect simulation output, saves,
// simulation hashes or LREC1 checkpoints. Parsing and evaluation never throw.
enum class ProfileError : std::uint8_t {
    None,
    Malformed,
    UnknownKey,
    OutOfRange,
    Duplicate,
    TooLarge,
    FutureVersion
};

std::string_view ProfileErrorName(ProfileError error);

struct SimBudgetProfile {
    std::optional<Luminumbra::world::RegionSchedulerConfig> region_scheduler;
    std::map<std::string, std::uint64_t> stage_max_work;       // stage name -> per-tick work cap
    std::map<std::string, std::uint64_t> residency_max_bytes;  // class name -> peak byte cap
};

struct ProfileParse {
    ProfileError error = ProfileError::None;
    std::string detail;  // short human message naming the offending key when there is one
    SimBudgetProfile profile;
};

inline constexpr std::size_t kMaxProfileBytes = 64 * 1024;
inline constexpr std::size_t kMaxProfileStages = 64;
inline constexpr std::size_t kMaxProfileResidencyClasses = 32;

ProfileParse ParseSimBudgetProfile(std::string_view json) noexcept;

// Returns the default configuration when the profile has no region_scheduler section.
Luminumbra::world::RegionSchedulerConfig ToRegionSchedulerConfig(const SimBudgetProfile& profile);

struct StageTick {
    std::string stage;
    std::uint64_t tick = 0;
    std::uint64_t work = 0;
};

struct ResidencySample {
    std::string cls;
    std::uint64_t peak_bytes = 0;
};

struct BudgetViolation {
    enum class Kind : std::uint8_t { Stage, Residency };
    Kind kind;
    std::string name;
    std::uint64_t tick = 0;  // always 0 for Residency
    std::uint64_t observed = 0;
    std::uint64_t limit = 0;
};

struct BudgetVerdict {
    bool no_violation = true;       // no observed value exceeded a cap
    bool coverage_complete = true;  // every profile stage and residency class was observed
    std::vector<BudgetViolation> violations;  // sorted by (kind, name, tick)
    std::vector<std::string> unevaluated;     // "stage:<name>" / "residency:<class>", sorted
};

BudgetVerdict EvaluateSimBudget(const SimBudgetProfile& profile, std::span<const StageTick> ticks,
                                std::span<const ResidencySample> samples);

}  // namespace luminumbra::simulation
