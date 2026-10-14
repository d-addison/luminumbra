#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace luminumbra::ai {
class CreatureSpeciesRegistry;
}
namespace luminumbra::foliage {
class SpeciesRegistry;
}

namespace Luminumbra::world {

// Versioned conservative defaults, resolved before content adoption.
struct CreatureDistantPolicy {
    std::uint32_t coarse_need_rate_ppm = 0;
    std::uint32_t coarse_travel_m_per_cadence = 0;
    std::uint16_t promote_distance_m = 512;
    std::uint32_t lifespan_cadences = 0;
    std::uint32_t reproduce_cadences = 0;
    bool operator==(const CreatureDistantPolicy&) const = default;
};
struct PlantDistantPolicy {
    std::uint8_t max_cadence_shift = 8;
    std::uint32_t resume_cap_ticks = 36000;
    bool operator==(const PlantDistantPolicy&) const = default;
};
inline constexpr CreatureDistantPolicy kDefaultCreaturePolicy{};
inline constexpr PlantDistantPolicy kDefaultPlantPolicy{};
inline constexpr std::uint16_t kDistantDefaultsVersion = 1;
inline constexpr std::size_t kMaxContentDocumentBytes = 1048576;
inline constexpr std::size_t kMaxSpeciesDefinitions = 256;
inline constexpr std::size_t kMaxCapturedDefinitionBytes = 16 * 1048576;
inline constexpr std::size_t kMaxContentDepth = 32;
inline constexpr std::uint16_t kDistantAlgorithmVersion = 1;
inline constexpr std::string_view kDefaultPopulationContract = "none:v1";

// Compare names by unsigned UTF-8 bytes, independently of the platform's char signedness.
struct ContentNameLess {
    bool operator()(std::string_view left, std::string_view right) const noexcept;
};
using SpeciesNames = std::set<std::string, ContentNameLess>;
using CreaturePolicies = std::map<std::string, CreatureDistantPolicy, ContentNameLess>;
using PlantPolicies = std::map<std::string, PlantDistantPolicy, ContentNameLess>;

enum class ContentReadStatus {
    Found,
    Absent,
    Error
};
// Deterministic read-boundary injection for filesystem regression coverage. Production is empty.
using ContentReadHook = std::function<void(const std::filesystem::path&)>;
struct ContentReadBoundaries {
    ContentReadHook after_parent_check;
    ContentReadHook before_open;
    ContentReadHook during_read;
};
struct ContentReadResult {
    ContentReadStatus status = ContentReadStatus::Error;
    std::string bytes;
    std::string error;
};
[[nodiscard]] ContentReadResult
ReadSimulationDocument(const std::filesystem::path& path,
                       bool optional = false,
                       const ContentReadHook& after_read = {},
                       const ContentReadBoundaries& boundaries = {});

// Parsed authored entries only; resolution supplies the single compiled table above.
class DistantPolicyOverrides {
public:
    [[nodiscard]] const CreaturePolicies& creatures() const noexcept {
        return m_creatures;
    }
    [[nodiscard]] const PlantPolicies& plants() const noexcept {
        return m_plants;
    }
    static bool parse(std::string_view bytes, DistantPolicyOverrides& out, std::string& error);
    static ContentReadStatus load(const std::filesystem::path& path,
                                  DistantPolicyOverrides& out,
                                  std::string& error,
                                  const ContentReadHook& after_read = {});
    [[nodiscard]] std::string canonical_bytes() const;
    bool operator==(const DistantPolicyOverrides&) const = default;

private:
    CreaturePolicies m_creatures;
    PlantPolicies m_plants;
};
struct ResolvedDistantPolicy {
    std::uint16_t defaults_version = kDistantDefaultsVersion;
    CreaturePolicies creatures;
    PlantPolicies plants;
    std::map<std::uint16_t, PlantDistantPolicy> plant_ids;
    [[nodiscard]] const PlantDistantPolicy& plant(std::uint16_t species_id) const;
    [[nodiscard]] std::string canonical_bytes() const;
    bool operator==(const ResolvedDistantPolicy&) const = default;
};
struct CapturedSpeciesDefinition {
    std::string name;
    std::uint16_t species_id = 0;
    std::string bytes;
};
struct SimulationContentIdentity {
    std::uint16_t encoding_version = 1;
    std::uint16_t defaults_version = kDistantDefaultsVersion;
    std::uint16_t algorithm_version = kDistantAlgorithmVersion;
    std::uint64_t effective_policy_digest = 0;
    std::uint64_t creature_definitions_digest = 0;
    std::uint64_t plant_definitions_digest = 0;
    std::uint64_t population_digest = 0;
    static constexpr std::size_t kEncodedBytes = 40;
    [[nodiscard]] std::string encode() const;
    static bool decode(std::string_view bytes, SimulationContentIdentity& out);
    bool operator==(const SimulationContentIdentity&) const = default;
};
struct SimulationContent {
    std::vector<CapturedSpeciesDefinition> creature_definitions, plant_definitions;
    std::shared_ptr<const luminumbra::ai::CreatureSpeciesRegistry> creatures;
    std::shared_ptr<const luminumbra::foliage::SpeciesRegistry> plants;
    ResolvedDistantPolicy policy;
    SimulationContentIdentity identity;
};
[[nodiscard]] std::uint16_t SimulationSpeciesId(std::string_view name);
[[nodiscard]] std::uint64_t SimulationContentDigest(std::string_view bytes);
bool ResolveDistantPolicy(const DistantPolicyOverrides& overrides,
                          const std::vector<CapturedSpeciesDefinition>& creatures,
                          const std::vector<CapturedSpeciesDefinition>& plants,
                          ResolvedDistantPolicy& out,
                          std::string& error);
[[nodiscard]] SimulationContentIdentity
BuildSimulationContentIdentity(const SimulationContent& content,
                               std::string_view callback_contract = kDefaultPopulationContract,
                               std::string_view population_configuration = {});
// Captures/validates every definition before invoking either legacy registry parser.
// Error leaves out unchanged; no simulation mutation, allocation of IDs, or fallback on Error.
bool AdmitSimulationContent(const std::filesystem::path& runtime_root,
                            SimulationContent& out,
                            std::string& error,
                            std::string_view callback_contract = kDefaultPopulationContract,
                            std::string_view population_configuration = {},
                            const ContentReadHook& after_read = {});

} // namespace Luminumbra::world
