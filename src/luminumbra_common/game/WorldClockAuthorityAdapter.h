#pragma once

#include "../core/ModuleRegistry.h"
#include "../persistence/AuthorityContributor.h"
#include "../world/WorldClock.h"

#include <array>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace Luminumbra::Game {

// Legacy world metadata implies this built-in module identity. Keep its clock
// bytes in the established ecology/checkpoint slot, without an extension suffix
// or a new save record. The adapter borrows the clock for one owner-thread call.
class WorldClockAuthorityAdapter final : public Persistence::IAuthorityMetadataContributor {
public:
    static constexpr std::string_view kModuleId = "luminumbra.world_clock.v1";
    static constexpr std::string_view kContributorId = "luminumbra:world_clock";
    static constexpr std::array<std::string_view, 1> kRequiredModules{kModuleId};

    explicit WorldClockAuthorityAdapter(world::WorldClock& clock)
        : m_clock(clock)
        , m_bytes(clock.canonical_bytes()) {}

    [[nodiscard]] std::string_view contributor_id() const noexcept override {
        return kContributorId;
    }
    [[nodiscard]] std::string_view canonical_state() const noexcept override {
        return m_bytes;
    }
    void WriteMetadata(nlohmann::json& metadata) const override {
        m_clock.write_metadata(metadata);
    }
    [[nodiscard]] bool PrepareRestore(const nlohmann::json& metadata, std::string& error) override {
        if (!world::WorldClock::from_metadata(metadata, m_prepared, error)) {
            return false;
        }
        m_preparedBytes = m_prepared.canonical_bytes();
        return true;
    }
    void CommitRestore() noexcept override {
        m_clock = m_prepared;
        m_bytes.swap(m_preparedBytes);
    }

    // The owning legacy format supplies the required identity. Each operation
    // registers exactly one module and contributor; no simulation-tick work.
    static bool
    ReadMetadata(const nlohmann::json& metadata, world::WorldClock& clock, std::string& error) {
        WorldClockAuthorityAdapter adapter(clock);
        ModuleRegistry modules;
        Persistence::AuthorityContributorRegistry contributors;
        Register(adapter, modules, contributors);
        return contributors.RestoreMetadata(modules, kRequiredModules, metadata, error);
    }
    static void WriteMetadata(const world::WorldClock& clock, nlohmann::json& metadata) {
        auto snapshot = clock;
        WorldClockAuthorityAdapter adapter(snapshot);
        ModuleRegistry modules;
        Persistence::AuthorityContributorRegistry contributors;
        Register(adapter, modules, contributors);
        if (!contributors.WriteMetadata(modules, kRequiredModules, metadata)) {
            throw std::invalid_argument("required world clock module unavailable");
        }
    }
    [[nodiscard]] static std::string CanonicalBytes(const world::WorldClock& clock) {
        auto snapshot = clock;
        WorldClockAuthorityAdapter adapter(snapshot);
        ModuleRegistry modules;
        Persistence::AuthorityContributorRegistry contributors;
        Register(adapter, modules, contributors);
        return std::string(
            contributors.LegacyCanonicalState(modules, kRequiredModules, kContributorId));
    }

private:
    static void Register(WorldClockAuthorityAdapter& adapter,
                         ModuleRegistry& modules,
                         Persistence::AuthorityContributorRegistry& contributors) {
        if (modules.register_module({.id = std::string(kModuleId),
                                     .provides = {},
                                     .requires_services = {},
                                     .startup = nullptr,
                                     .shutdown = nullptr,
                                     .context = nullptr}) != ModuleResult::Ok ||
            !contributors.RegisterMetadata(adapter, kModuleId) ||
            modules.startup() != ModuleResult::Ok) {
            throw std::invalid_argument("world clock authority registration failed");
        }
    }

    world::WorldClock& m_clock;
    // Clock records are bounded to a 15-byte tag plus at most 28 payload bytes.
    std::string m_bytes;
    world::WorldClock m_prepared;
    std::string m_preparedBytes;
};

} // namespace Luminumbra::Game
