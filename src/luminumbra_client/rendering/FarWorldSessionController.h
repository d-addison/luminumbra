#pragma once

#include "FarWorldSessionLimits.h"

namespace Luminumbra::Rendering {
// Owner-thread refresh orchestration, inactive until explicitly configured.
// The host fences publication during Pump and exclusively delegates source and
// frozen pumping to this owner. Renderer scheduling remains with the host.
// Destroy/cancel before renderer teardown; injected clocks outlive the binding.
class FarWorldSessionController final {
public:
    enum class Phase {
        Idle,
        Source,
        Planning,
        Admission,
        Waiting,
        Complete,
        Refused
    };
    explicit FarWorldSessionController(FarWorldRenderer& renderer);
    ~FarWorldSessionController();
    FarWorldSessionController(const FarWorldSessionController&) = delete;
    FarWorldSessionController& operator=(const FarWorldSessionController&) = delete;
    FarWorldRefusal Configure(JobSystem& jobs,
                              std::uint64_t epoch,
                              const World::FarWorldAuthorityProducer::FrozenPolicy& policy,
                              const FarWorldSessionQualificationLimits& limits);
    // Supply this configuration to the saved session's producer attachment.
    // Its owning pool is the renderer's admitted overall source/control domain.
    World::FarWorldAuthorityConfig SourceConfig() const;
    FarWorldRefusal Attach(const std::shared_ptr<World::FarWorldAuthorityProducer>& producer,
                           const std::shared_ptr<const Systems::SHIELD_WorldSystem>& world);
    // Canonical wanted set, at most limits.tiles (and 512) keys. During an active
    // refresh this replaces only the single successor, never the accepted source.
    // Empty/invalid requests preserve both active and queued transactions.
    bool Request(std::span<const World::FarTileKey> keys);
    // At most one pump per increasing prepared-frame number. Source copy/read
    // remain synchronous bounded calls; their entire elapsed cost is measured.
    Phase Pump(std::uint64_t frame);
    void Cancel();
    Phase phase() const;
    const FarWorldRefreshWork& work() const;
    World::FarWorldAuthorityProducer::FrozenResult failure() const;
    // Retires handoff first, then prunes only keys absent from its complete
    // snapshot. No staging occurs between those steps; pending attempts pin all.
    std::size_t RetireIdleHistory();
    // Narrow const history diagnostic, independent of mutable planner internals.
    bool Retains(const World::FarTileKey& key) const;

private:
    struct Impl;
    void CheckOwner() const;
    void Refuse(FarWorldRefusal refusal,
                const World::FarWorldAuthorityProducer::FrozenResult& failure = {});
    void Begin();
    void PlanAdmission();
    FarWorldRenderer& renderer_;
    const std::thread::id owner_ = std::this_thread::get_id();
    std::shared_ptr<World::FarVolumeMemoryPool> pool_;
    World::FarVolumeMemoryLease charge_;
    std::shared_ptr<Impl> impl_;
    Phase phase_ = Phase::Idle;
    FarWorldRefusal refusal_ = FarWorldRefusal::None;
    FarWorldRefreshWork work_;
    World::FarWorldAuthorityProducer::FrozenResult failure_;
};
} // namespace Luminumbra::Rendering
