#include "NetworkStateHash.h"

#include "NetworkLoopbackAuthority.h"
#include "ecs/EntitySnapshot.h"
#include "persistence/WorldPersistenceRoundtrip.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace luminumbra::network {
namespace {

constexpr const char* kSchema = "luminumbra.network.state_hash.v1";
constexpr const char* kSourcePath = "src/luminumbra_common/network/NetworkStateHash.cpp";
constexpr const char* kHeaderPath = "src/luminumbra_common/network/NetworkStateHash.h";
constexpr const char* kHashApi = "BuildNetworkStateHashFixture";
constexpr const char* kValidationApi = "NetworkStateHashMeetsBaseline";
constexpr const char* kArtifactWriter = "WriteNetworkStateHashArtifact";
constexpr const char* kStateContract = "authoritative_sorted_state_per_tick";
constexpr const char* kOrderContract = "tick_ascending_sorted_state_fields";
constexpr const char* kHashAlgorithm = "fnv1a_64_canonical_state_string";

constexpr std::uint64_t kFnvOffset = 14695981039346656037ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

std::uint64_t Fnv1a64(const std::string& value) {
    std::uint64_t hash = kFnvOffset;
    for (const unsigned char byte : value) {
        hash ^= byte;
        hash *= kFnvPrime;
    }
    return hash;
}

std::string Hex64(const std::uint64_t value) {
    std::ostringstream out;
    out << std::hex << std::setfill('0') << std::setw(16) << value;
    return out.str();
}

const char* BoolLiteral(const bool value) {
    return value ? "true" : "false";
}

// Canonical per-tick state string: field names sorted, values from the
// authoritative server decision stream, suffixed with the durable entity ids
// (already sorted ascending by the entity snapshot order contract) and the
// stable world hash from the persistence gate.
std::string CanonicalStateString(const NetworkLoopbackState& state,
                                 const std::vector<std::string>& durableEntityIds,
                                 const std::string& worldHash) {
    std::ostringstream out;
    out << "authoritative_revision=" << state.authoritativeRevision
        << "|position_x_mm=" << state.positionXMm << "|position_y_mm=" << state.positionYMm
        << "|tick=" << state.tick << "|entities=";
    for (std::size_t i = 0; i < durableEntityIds.size(); ++i) {
        if (i != 0u) {
            out << ',';
        }
        out << durableEntityIds[i];
    }
    out << "|world_hash=" << worldHash;
    return out.str();
}

struct ReplayTrace {
    std::vector<NetworkStateHashTick> ticks;
    std::string finalStateHash;
};

ReplayTrace BuildReplayTrace(const NetworkLoopbackConvergenceReport& loopback,
                             const std::vector<std::string>& durableEntityIds,
                             const std::string& worldHash) {
    ReplayTrace trace;
    std::uint64_t chained = kFnvOffset;
    for (const NetworkLoopbackDecision& decision : loopback.decisions) {
        if (!decision.accepted) {
            continue;
        }
        NetworkStateHashTick tick;
        tick.tick = decision.authoritativeState.tick;
        tick.authoritativeRevision = decision.authoritativeState.authoritativeRevision;
        tick.positionXMm = decision.authoritativeState.positionXMm;
        tick.positionYMm = decision.authoritativeState.positionYMm;
        tick.canonicalState =
            CanonicalStateString(decision.authoritativeState, durableEntityIds, worldHash);
        const std::uint64_t tick_hash = Fnv1a64(tick.canonicalState);
        tick.stateHash = Hex64(tick_hash);
        chained ^= tick_hash;
        chained *= kFnvPrime;
        trace.ticks.push_back(tick);
    }
    trace.finalStateHash = Hex64(chained);
    return trace;
}

} // namespace

NetworkStateHashReport
BuildNetworkStateHashFixture(const Luminumbra::Ecs::EntityRegistrySnapshot& entities_fixture,
                             const std::string& buildPreset) {
    NetworkStateHashReport report;
    report.schema = kSchema;
    report.buildPreset = buildPreset;
    report.source = kSourcePath;
    report.header = kHeaderPath;
    report.hashApi = kHashApi;
    report.validationApi = kValidationApi;
    report.artifactWriter = kArtifactWriter;
    report.stateContract = kStateContract;
    report.orderContract = kOrderContract;
    report.hashAlgorithm = kHashAlgorithm;

    const Luminumbra::Persistence::WorldHashAnalysis world_hash =
        Luminumbra::Persistence::BuildWorldHashAnalysis(buildPreset);
    report.worldHash = world_hash.hash;

    Luminumbra::Ecs::EntityRegistrySnapshot entities = entities_fixture;
    Luminumbra::Ecs::SortEntityRegistrySnapshot(entities);
    for (const auto& entity : entities.entities) {
        report.durableEntityIds.push_back(std::to_string(entity.entity_id));
    }

    const NetworkLoopbackConvergenceReport loopback =
        BuildNetworkLoopbackConvergenceFixture(buildPreset);
    const ReplayTrace first = BuildReplayTrace(loopback, report.durableEntityIds, report.worldHash);

    const NetworkLoopbackConvergenceReport replayed_loopback =
        BuildNetworkLoopbackConvergenceFixture(buildPreset);
    const ReplayTrace second =
        BuildReplayTrace(replayed_loopback, report.durableEntityIds, report.worldHash);

    report.ticks = first.ticks;
    report.tickCount = static_cast<std::uint32_t>(first.ticks.size());
    report.finalStateHash = first.finalStateHash;
    report.replayFinalStateHash = second.finalStateHash;

    report.deterministicReplay =
        first.ticks.size() == second.ticks.size() &&
        std::equal(first.ticks.begin(),
                   first.ticks.end(),
                   second.ticks.begin(),
                   [](const NetworkStateHashTick& lhs, const NetworkStateHashTick& rhs) {
                       return lhs.stateHash == rhs.stateHash && lhs.tick == rhs.tick;
                   }) &&
        first.finalStateHash == second.finalStateHash;

    report.monotonicTicks =
        std::is_sorted(report.ticks.begin(),
                       report.ticks.end(),
                       [](const NetworkStateHashTick& lhs, const NetworkStateHashTick& rhs) {
                           return lhs.tick < rhs.tick;
                       });

    const bool world_hash_present = world_hash.passed && !report.worldHash.empty();
    const bool entities_present = !report.durableEntityIds.empty();
    const bool loopback_baseline = NetworkLoopbackAuthorityMeetsBaseline(loopback);
    const bool ticks_present = report.tickCount >= 5u;
    const bool hashes_present =
        std::all_of(report.ticks.begin(), report.ticks.end(), [](const NetworkStateHashTick& tick) {
            return tick.stateHash.size() == 16u;
        });

    report.checks = {
        {"world hash from persistence gate is stable and present", world_hash_present},
        {"durable entity ids come from the sorted entity snapshot", entities_present},
        {"loopback authority fixture meets its baseline", loopback_baseline},
        {"every accepted authoritative tick produces a state hash",
         ticks_present && hashes_present},
        {"per-tick hashes are identical across a full replay", report.deterministicReplay},
        {"authoritative ticks hash in ascending tick order", report.monotonicTicks},
    };

    report.passed = std::all_of(report.checks.begin(),
                                report.checks.end(),
                                [](const NetworkStateHashCheck& check) { return check.passed; });
    return report;
}

std::string SerializeNetworkStateHashJson(const NetworkStateHashReport& report) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema\": \"" << report.schema << "\",\n";
    out << "  \"passed\": " << BoolLiteral(report.passed) << ",\n";
    out << "  \"build_preset\": \"" << report.buildPreset << "\",\n";
    out << "  \"network\": {\n";
    out << "    \"source\": \"" << report.source << "\",\n";
    out << "    \"header\": \"" << report.header << "\",\n";
    out << "    \"hash_api\": \"" << report.hashApi << "\",\n";
    out << "    \"validation_api\": \"" << report.validationApi << "\",\n";
    out << "    \"artifact_writer\": \"" << report.artifactWriter << "\",\n";
    out << "    \"state_contract\": \"" << report.stateContract << "\",\n";
    out << "    \"order_contract\": \"" << report.orderContract << "\"\n";
    out << "  },\n";
    out << "  \"state_hash\": {\n";
    out << "    \"hash_algorithm\": \"" << report.hashAlgorithm << "\",\n";
    out << "    \"world_hash\": \"" << report.worldHash << "\",\n";
    out << "    \"durable_entity_id_count\": " << report.durableEntityIds.size() << ",\n";
    out << "    \"durable_entity_ids\": [";
    for (std::size_t i = 0; i < report.durableEntityIds.size(); ++i) {
        if (i != 0u) {
            out << ", ";
        }
        out << '"' << report.durableEntityIds[i] << '"';
    }
    out << "],\n";
    out << "    \"tick_count\": " << report.tickCount << ",\n";
    out << "    \"deterministic_replay\": " << BoolLiteral(report.deterministicReplay) << ",\n";
    out << "    \"monotonic_ticks\": " << BoolLiteral(report.monotonicTicks) << ",\n";
    out << "    \"final_state_hash\": \"" << report.finalStateHash << "\",\n";
    out << "    \"replay_final_state_hash\": \"" << report.replayFinalStateHash << "\"\n";
    out << "  },\n";
    out << "  \"ticks\": [\n";
    for (std::size_t i = 0; i < report.ticks.size(); ++i) {
        const NetworkStateHashTick& tick = report.ticks[i];
        out << "    {\"tick\": " << tick.tick
            << ", \"authoritative_revision\": " << tick.authoritativeRevision
            << ", \"position_x_mm\": " << tick.positionXMm
            << ", \"position_y_mm\": " << tick.positionYMm << ", \"state_hash\": \""
            << tick.stateHash << "\"}";
        out << (i + 1u < report.ticks.size() ? ",\n" : "\n");
    }
    out << "  ],\n";
    out << "  \"checks\": [\n";
    for (std::size_t i = 0; i < report.checks.size(); ++i) {
        out << "    {\"name\": \"" << report.checks[i].name
            << "\", \"passed\": " << BoolLiteral(report.checks[i].passed) << "}";
        out << (i + 1u < report.checks.size() ? ",\n" : "\n");
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

bool NetworkStateHashMeetsBaseline(const NetworkStateHashReport& report) {
    return report.passed && report.schema == kSchema && report.deterministicReplay &&
           report.monotonicTicks && report.tickCount >= 5u && !report.worldHash.empty() &&
           !report.durableEntityIds.empty() && report.finalStateHash == report.replayFinalStateHash;
}

bool WriteNetworkStateHashArtifact(const std::string& path,
                                   const Luminumbra::Ecs::EntityRegistrySnapshot& entities,
                                   const std::string& buildPreset) {
    const NetworkStateHashReport report = BuildNetworkStateHashFixture(entities, buildPreset);
    const std::filesystem::path output_path(path);
    std::error_code ec;
    std::filesystem::create_directories(output_path.parent_path(), ec);
    std::ofstream output(output_path, std::ios::binary);
    if (!output) {
        return false;
    }
    output << SerializeNetworkStateHashJson(report);
    return NetworkStateHashMeetsBaseline(report);
}

} // namespace luminumbra::network
