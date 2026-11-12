#include <gtest/gtest.h>

#include "../support/IndexedSaveFixture.h"
#include "../support/UniqueTempPath.h"
#include "core/FilesystemPath.h"
#include "persistence/PlantSnapshot.h"
#include "persistence/SaveTransaction.h"
#include "persistence/WorldSaveService.h"
#include "world/WorldStreamingState.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
namespace P = Luminumbra::Persistence;
namespace W = Luminumbra::world;
namespace fs = std::filesystem;
using Service = P::WorldSaveService;
using Tree = std::map<std::string, std::string>;

constexpr std::string_view kPlants = "chunks/region/plant-entities.json";
constexpr std::string_view kSidecar = "chunks/region/plant-identities.json";

P::PlantSnapshot Roster(std::uint64_t cursor, bool owned = false) {
    Luminumbra::Ecs::EntitySnapshotRecord plant;
    plant.entity_id = 1;
    plant.name = "plant";
    plant.components = {{"Transform", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}}},
                        {"PlantGenome", {{"genes", {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}}}},
                        {"PlantGrowth",
                         {{"species_id", 1u},
                          {"stage", 0u},
                          {"quality", 100u},
                          {"growth_points", 0u},
                          {"stress_points", 0u},
                          {"tended", 0u},
                          {"planted_tick", 0u},
                          {"last_tick", 0u}}}};
    if (owned)
        plant.components.push_back(
            {"PlantRegion", {{"owner_x", 0}, {"owner_z", 0}, {"region_stamp", 0u}}});
    return {.next_id = cursor, .entities = {plant}};
}

W::ActiveRegionLedger Ledger(std::uint64_t cursor) {
    W::ActiveRegionLedger ledger;
    ledger.mark_edited({0, 0}, W::WorldClock{});
    EXPECT_TRUE(ledger.reserve_persistent_ids(cursor));
    return ledger;
}

std::string Encode(const P::PlantSnapshot& plants) {
    std::string bytes;
    std::string error;
    EXPECT_TRUE(P::EncodePlantSnapshot(plants, bytes, error)) << error;
    return bytes;
}

Tree Files(const fs::path& root) {
    Tree files;
    if (!fs::exists(root))
        return files;
    // Test trees contain at most four singleton payloads, their index and journal.
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file())
            continue;
        std::ifstream input(entry.path(), std::ios::binary);
        const auto relative = entry.path().lexically_relative(root);
        files.emplace(Luminumbra::Filesystem::GenericUtf8PathString(relative),
                      std::string(std::istreambuf_iterator<char>(input), {}));
    }
    return files;
}

void Put(const fs::path& path, const std::string& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.good());
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.flush();
    ASSERT_TRUE(output.good());
}

bool Publish(const fs::path& root, std::uint64_t cursor, bool regions) {
    const auto ledger = Ledger(cursor);
    return Luminumbra::Test::PublishPlantSnapshotSave(
        root, Roster(cursor, regions), regions ? &ledger : nullptr, nullptr, cursor);
}

void ExpectRefusal(const fs::path& root) {
    const auto before = Files(root);
    auto plants = Roster(97);
    const auto sentinel = Encode(plants);
    std::vector<std::string> errors;
    EXPECT_FALSE(Service::load_plant_snapshot(plants, root, &errors));
    EXPECT_EQ(Encode(plants), sentinel);
    EXPECT_FALSE(errors.empty());
    errors.clear();
    EXPECT_FALSE(Service::validate_save(root, &errors));
    EXPECT_FALSE(errors.empty());
    EXPECT_EQ(Files(root), before);
    P::SaveTransaction transaction(root, 0, {}, &Service::member_digest);
    EXPECT_FALSE(transaction.begin(&errors));
    EXPECT_EQ(Files(root), before);
}

TEST(PlantSnapshotCommit, BothRegionModesCommitReopenAndRetainEmptyExhaustedCursor) {
    const Luminumbra::TestSupport::ScopedTempFile temp("plant_commit_", ".unused");
    const P::PlantSnapshotContract contract;
    for (const bool regions : {false, true}) {
        const auto root = temp.Path().parent_path() / (regions ? "on" : "off");
        ASSERT_TRUE(Publish(root, 7, regions));
        P::PlantSnapshot loaded;
        ASSERT_TRUE(Service::load_plant_snapshot(loaded, root));
        EXPECT_EQ(Encode(loaded), Encode(Roster(7, regions)));
        {
            P::SaveTransaction transaction(root, 0, {}, &Service::member_digest);
            ASSERT_TRUE(transaction.begin(nullptr));
            ASSERT_TRUE(transaction.protect({std::string(kSidecar)}, nullptr));
            ASSERT_TRUE(Service::write_member_bytes(root, kSidecar, "{}"));
            ASSERT_TRUE(Service::remove_member(root, kSidecar));
            ASSERT_TRUE(transaction.commit(nullptr));
        }
        Luminumbra::WorldStreamingState world;
        std::vector<std::string> errors;
        ASSERT_TRUE(Service{}.load_world(world, root, errors));
        EXPECT_TRUE(errors.empty());
        for (const auto cursor : {std::uint64_t{31}, W::ActiveRegionLedger::kPersistentIdLimit}) {
            const auto ledger = Ledger(cursor);
            const P::PlantSnapshot empty{.next_id = cursor, .entities = {}};
            const auto empty_bytes = Encode(empty);
            ASSERT_TRUE(Luminumbra::Test::PublishPlantSnapshotSave(
                root, empty, regions ? &ledger : nullptr));
            ASSERT_TRUE(Service::load_plant_snapshot(loaded, root));
            EXPECT_EQ(Encode(loaded), Encode(empty));
            EXPECT_TRUE(Service::validate_save(root));
            P::SnapshotIndex index;
            ASSERT_TRUE(P::read_snapshot_index(root, index, nullptr));
            const auto member = std::find_if(index.members.begin(),
                                             index.members.end(),
                                             [](const auto& item) { return item.path == kPlants; });
            ASSERT_NE(member, index.members.end());
            EXPECT_TRUE(member->present);
            EXPECT_EQ(member->bytes, empty_bytes.size());
            EXPECT_EQ(member->fnv64, P::fnv1a64_bytes(empty_bytes.data(), empty_bytes.size()));
            EXPECT_TRUE(std::none_of(index.members.begin(),
                                     index.members.end(),
                                     [](const auto& item) { return item.path == kSidecar; }));
            EXPECT_FALSE(fs::exists(Service::plant_identities_path(root)));
        }
    }
}

TEST(PlantSnapshotCommit, MissingUnlistedTombstonedAndReintroducedMembersRefuse) {
    const Luminumbra::TestSupport::ScopedTempFile temp("plant_inventory_", ".unused");
    const P::PlantSnapshotContract contract;
    for (const bool regions : {false, true}) {
        for (unsigned corruption = 0; corruption < 13; ++corruption) {
            SCOPED_TRACE(corruption);
            SCOPED_TRACE(regions);
            const auto root =
                temp.Path().parent_path() / ((regions ? "on" : "off") + std::to_string(corruption));
            ASSERT_TRUE(Publish(root, 7, regions));
            P::SnapshotIndex index;
            ASSERT_TRUE(P::read_snapshot_index(root, index, nullptr));
            if (corruption == 0) {
                ASSERT_TRUE(fs::remove(Service::plant_entities_path(root)));
            } else if (corruption == 1 || corruption == 2) {
                std::erase_if(index.members,
                              [](const auto& member) { return member.path == kPlants; });
                if (corruption == 2) {
                    ASSERT_TRUE(fs::remove(Service::plant_entities_path(root)));
                }
            } else if (corruption == 3) {
                for (auto& member : index.members)
                    if (member.path == kPlants)
                        member.present = false;
            } else if (corruption == 4 || corruption == 5) {
                // Even a listed, correctly fingerprinted sidecar is obsolete in v2.
                const std::string sidecar = "{}";
                if (corruption == 4)
                    Put(Service::plant_identities_path(root), sidecar);
                index.members.push_back(
                    {.path = std::string(kSidecar),
                     .cls = P::MemberClass::Authoritative,
                     .present = corruption == 4,
                     .bytes = corruption == 4 ? sidecar.size() : 0,
                     .fnv64 =
                         corruption == 4 ? P::fnv1a64_bytes(sidecar.data(), sidecar.size()) : 0,
                     .generation = index.generation});
                std::sort(
                    index.members.begin(),
                    index.members.end(),
                    [](const auto& left, const auto& right) { return left.path < right.path; });
            } else if (corruption == 6) {
                Put(Service::plant_identities_path(root), "{}");
            } else if (corruption == 7) {
                Put(Service::plant_entities_path(root), "{");
            } else if (corruption == 8) {
                ASSERT_TRUE(fs::remove(P::snapshot_index_path(root)));
            } else if (corruption == 9) {
                fs::create_directory(Service::plant_identities_path(root));
            } else if (corruption == 10) {
                ASSERT_TRUE(fs::remove(Service::plant_entities_path(root)));
                fs::create_directory(Service::plant_entities_path(root));
            } else if (corruption == 11) {
                // Seal a valid but mismatched cursor: codec/cross-member validation,
                // rather than a checksum error, must reject it.
                const auto bytes = Ledger(8).encode();
                Put(Service::active_regions_path(root), bytes);
                std::erase_if(index.members, [](const auto& member) {
                    return member.path == "chunks/region/active-regions.arl";
                });
                index.members.push_back({.path = "chunks/region/active-regions.arl",
                                         .cls = P::MemberClass::Authoritative,
                                         .present = true,
                                         .bytes = bytes.size(),
                                         .fnv64 = P::fnv1a64_bytes(bytes.data(), bytes.size()),
                                         .generation = index.generation});
                std::sort(
                    index.members.begin(),
                    index.members.end(),
                    [](const auto& left, const auto& right) { return left.path < right.path; });
            } else {
                Put(Service::active_regions_path(root), "partial ledger");
            }
            if (corruption != 8)
                Put(P::snapshot_index_path(root), P::encode_snapshot_index(index));
            ExpectRefusal(root);
        }
    }
}

TEST(PlantSnapshotCommit, PreparationAndContextRefusalPublishNothingAndCannotReuseStaleBytes) {
    const Luminumbra::TestSupport::ScopedTempFile temp("plant_prepare_", ".unused");
    const P::PlantSnapshotContract contract;
    for (const bool regions : {false, true}) {
        const auto root = temp.Path().parent_path() / (regions ? "on" : "off");
        ASSERT_TRUE(Publish(root, 7, regions));
        const auto before = Files(root);
        auto invalid = Roster(7, regions);
        invalid.entities.push_back(invalid.entities.front());
        const auto ledger = Ledger(7);
        const auto fresh = root / "fresh";
        EXPECT_FALSE(Luminumbra::Test::PublishPlantSnapshotSave(
            fresh, invalid, regions ? &ledger : nullptr));
        EXPECT_FALSE(fs::exists(fresh));
        EXPECT_FALSE(
            Luminumbra::Test::PublishPlantSnapshotSave(root, invalid, regions ? &ledger : nullptr));
        EXPECT_EQ(Files(root), before);
        Service::PreparedPlantSnapshotWrite prepared;
        ASSERT_TRUE(prepared.prepare(Roster(7, regions)));
        EXPECT_FALSE(prepared.prepare(invalid));
        {
            P::SaveTransaction transaction(root, 0, {}, &Service::member_digest);
            ASSERT_TRUE(transaction.begin(nullptr));
            EXPECT_FALSE(transaction.publish_plant_snapshot(prepared.bytes()));
            EXPECT_FALSE(transaction.member_is_protected(std::string(kPlants)));
        }
        EXPECT_EQ(Files(root), before);
        {
            P::SaveTransaction transaction(root, 0, {}, &Service::member_digest);
            ASSERT_TRUE(transaction.begin(nullptr));
            EXPECT_FALSE(transaction.publish_plant_snapshot(Encode(Roster(6, regions))));
            EXPECT_FALSE(transaction.member_is_protected(std::string(kPlants)));
        }
        EXPECT_EQ(Files(root), before);
        if (regions) {
            const auto mismatch = Ledger(8);
            EXPECT_FALSE(
                Luminumbra::Test::PublishPlantSnapshotSave(root, Roster(7, true), &mismatch));
            EXPECT_EQ(Files(root), before);
            {
                P::SaveTransaction transaction(root, 0, {}, &Service::member_digest);
                ASSERT_TRUE(transaction.begin(nullptr));
                EXPECT_FALSE(transaction.publish_plant_snapshot(Encode(Roster(8, true))));
                EXPECT_FALSE(transaction.member_is_protected(std::string(kPlants)));
            }
            EXPECT_EQ(Files(root), before);
        }
        auto output = Roster(97);
        std::stop_source stopped;
        stopped.request_stop();
        EXPECT_FALSE(Service::load_plant_snapshot(output, root, nullptr, stopped.get_token()));
        EXPECT_EQ(Encode(output), Encode(Roster(97)));
        EXPECT_EQ(Files(root), before);
    }
}

TEST(PlantSnapshotCommit, CommitRechecksMissingEnvelopeSidecarAndLedgerAfterPublication) {
    const Luminumbra::TestSupport::ScopedTempFile temp("plant_recheck_", ".unused");
    const P::PlantSnapshotContract contract;
    for (const bool regions : {false, true}) {
        for (unsigned corruption = 0; corruption < 4; ++corruption) {
            const auto root =
                temp.Path().parent_path() / ((regions ? "on" : "off") + std::to_string(corruption));
            ASSERT_TRUE(Publish(root, 7, regions));
            const auto before = Files(root);
            bool acknowledged = false;
            {
                P::SaveTransaction transaction(root, 0, {}, &Service::member_digest);
                ASSERT_TRUE(transaction.begin(nullptr));
                transaction.defer_until_commit([&acknowledged] { acknowledged = true; });
                ASSERT_TRUE(transaction.publish_plant_snapshot(Encode(Roster(7, regions))));
                if (corruption == 0) {
                    ASSERT_TRUE(Service::remove_member(root, kPlants));
                } else if (corruption == 1) {
                    ASSERT_TRUE(transaction.protect({std::string(kSidecar)}, nullptr));
                    ASSERT_TRUE(Service::write_member_bytes(root, kSidecar, "{}"));
                } else if (corruption == 2) {
                    ASSERT_TRUE(transaction.protect({"chunks/region/active-regions.arl"}, nullptr));
                    ASSERT_TRUE(Service::write_member_bytes(
                        root, "chunks/region/active-regions.arl", Ledger(8).encode()));
                } else {
                    ASSERT_TRUE(transaction.protect({"chunks/region/active-regions.arl"}, nullptr));
                    ASSERT_TRUE(Service::write_member_bytes(
                        root, "chunks/region/active-regions.arl", Ledger(6).encode()));
                    ASSERT_TRUE(
                        Service::write_member_bytes(root, kPlants, Encode(Roster(6, regions))));
                }
                if (corruption == 3) {
                    EXPECT_TRUE(Service::finalize_indexed_envelope(root));
                } else {
                    EXPECT_FALSE(Service::finalize_indexed_envelope(root));
                }
                EXPECT_FALSE(transaction.commit(nullptr));
            }
            EXPECT_FALSE(acknowledged);
            EXPECT_EQ(Files(root), before);
            EXPECT_TRUE(Service::validate_save(root));
        }
    }
}

TEST(PlantSnapshotCommit, ShippingV1OutputAndEmptyRemovalRemainUnchanged) {
    const Luminumbra::TestSupport::ScopedTempFile temp("plant_legacy_", ".unused");
    const auto root = temp.Path().parent_path();
    Luminumbra::Ecs::EntityRegistrySnapshot legacy;
    legacy.entities = Roster(7).entities;
    const auto expected = Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(legacy);
    ASSERT_TRUE(Service::save_plant_entities(legacy, root));
    EXPECT_EQ(Files(root), (Tree{{std::string(kPlants), expected}}));
    const auto before = Files(root);
    {
        Service::SaveFactsScope cached_legacy(root);
        ASSERT_TRUE(cached_legacy.validate(nullptr));
        {
            Service::SaveFactsScope nested(root);
            ASSERT_TRUE(nested.validate(nullptr));
            {
                const P::PlantSnapshotContract candidate;
                ExpectRefusal(root);
                EXPECT_FALSE(cached_legacy.validate(nullptr));
            }
            EXPECT_TRUE(nested.validate(nullptr));
        }
        EXPECT_TRUE(cached_legacy.validate(nullptr));
        EXPECT_TRUE(Service::save_plant_entities(legacy, root));
    }
    ASSERT_TRUE(Service::save_plant_entities(legacy, root));
    EXPECT_EQ(Files(root), before);
    ASSERT_TRUE(Service::save_plant_entities(Luminumbra::Ecs::EntityRegistrySnapshot{}, root));
    EXPECT_TRUE(Files(root).empty());
    EXPECT_FALSE(P::snapshot_index_exists(root));
}

TEST(PlantSnapshotCommit, CrossKindCollisionPartialGroundAndUnallocatedGroundRefuseBeforePlants) {
    const Luminumbra::TestSupport::ScopedTempFile temp("plant_ground_", ".unused");
    const P::PlantSnapshotContract contract;
    for (const bool regions : {false, true}) {
        for (unsigned corruption = 0; corruption < 3; ++corruption) {
            const auto root =
                temp.Path().parent_path() / ((regions ? "on" : "off") + std::to_string(corruption));
            ASSERT_TRUE(Publish(root, 7, regions));
            const auto before = Files(root);
            W::GroundObjectStore ground;
            ASSERT_TRUE(ground.insert(W::GroundObject{.id = corruption == 2 ? 7u : 1u, .type = 1}));
            {
                P::SaveTransaction transaction(root, 0, {}, &Service::member_digest);
                ASSERT_TRUE(transaction.begin(nullptr));
                ASSERT_TRUE(transaction.protect(
                    {"chunks/region/active-regions.arl", W::GroundObjectStore::kRelativePath},
                    nullptr));
                ASSERT_TRUE(Service::write_member_bytes(
                    root, "chunks/region/active-regions.arl", Ledger(7).encode()));
                ASSERT_TRUE(
                    Service::write_member_bytes(root,
                                                W::GroundObjectStore::kRelativePath,
                                                corruption == 1 ? "{" : ground.serialize()));
                EXPECT_FALSE(transaction.publish_plant_snapshot(Encode(Roster(7, regions))));
                EXPECT_FALSE(transaction.member_is_protected(std::string(kPlants)));
                EXPECT_FALSE(transaction.commit(nullptr));
            }
            EXPECT_EQ(Files(root), before);
            EXPECT_TRUE(Service::validate_save(root));
        }
    }
}

struct PublicationTrace {
    std::vector<std::string> steps;
    std::size_t stop_at = 0;
    bool committed = false;
    bool interrupted = false;
};

PublicationTrace& Trace() {
    static PublicationTrace trace;
    return trace;
}

bool PublicationStep(std::string_view step) {
    return step == "journal-published" || step == "before-replace" || step == "after-retire" ||
           step == "after-replace" || step == "before-delete" || step == "after-delete" ||
           step == "commit-point" || step == "journal-unlinked";
}

void TracePublication(const char* step, const fs::path&) {
    if (!PublicationStep(step))
        return;
    auto& trace = Trace();
    trace.steps.emplace_back(step);
    trace.committed = trace.committed || std::string_view(step) == "commit-point";
    if (trace.steps.size() == trace.stop_at) {
        trace.interrupted = true;
        P::set_snapshot_step_hook_for_testing(nullptr);
        throw std::runtime_error("interrupted candidate publication");
    }
}

class PublicationHook {
public:
    PublicationHook() {
        P::set_snapshot_step_hook_for_testing(&TracePublication);
    }
    ~PublicationHook() {
        P::set_snapshot_step_hook_for_testing(nullptr);
    }
    PublicationHook(const PublicationHook&) = delete;
    PublicationHook& operator=(const PublicationHook&) = delete;
};

TEST(PlantSnapshotCommit, EveryPublicationInterruptionPreservesTheDurableCommitInBothModes) {
    const Luminumbra::TestSupport::ScopedTempFile temp("plant_interrupt_", ".unused");
    const P::PlantSnapshotContract contract;
    for (const bool regions : {false, true}) {
        const auto baseline = temp.Path().parent_path() / (regions ? "on" : "off");
        ASSERT_TRUE(Publish(baseline, 7, regions));
        const auto before = Files(baseline);
        const auto sample = temp.Path().parent_path() / "sample";
        for (const auto& [path, bytes] : before)
            Put(sample / path, bytes);
        Trace() = {};
        {
            const PublicationHook hook;
            ASSERT_TRUE(Publish(sample, 8, regions));
        }
        const auto after = Files(sample);
        const auto step_count = Trace().steps.size();
        ASSERT_GT(step_count, 0u);
        // Four singleton payloads plus the index and bounded journal publications.
        ASSERT_LT(step_count, 128u);
        fs::remove_all(sample);
        for (std::size_t cut = 1; cut <= step_count; ++cut) {
            SCOPED_TRACE(cut);
            SCOPED_TRACE(regions);
            const auto root = temp.Path().parent_path() / "interrupted";
            for (const auto& [path, bytes] : before)
                Put(root / path, bytes);
            Trace() = {.steps = {}, .stop_at = cut, .committed = false, .interrupted = false};
            try {
                const PublicationHook hook;
                Publish(root, 8, regions);
            } catch (const std::runtime_error&) {}
            ASSERT_TRUE(Trace().interrupted);
            EXPECT_NE(P::recover_pending_journal(root, nullptr), P::RecoveryOutcome::Failed);
            EXPECT_EQ(Files(root), Trace().committed ? after : before);
            EXPECT_FALSE(P::journal_pending(root));
            EXPECT_TRUE(Service::validate_save(root));
            EXPECT_EQ(P::recover_pending_journal(root, nullptr), P::RecoveryOutcome::NothingToDo);
            ASSERT_TRUE(Publish(root, 9, regions));
            fs::remove_all(root);
        }
    }
}
} // namespace
