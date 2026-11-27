#include "persistence/RecoveryDurability.h"
#include "persistence/WorldSaveService.h"

#include "../support/UniqueTempPath.h"

#include <gtest/gtest.h>

#include <string>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

using Identity = std::array<std::uint64_t, 3>;
using Parents = std::array<std::filesystem::path, 4>;
constexpr Identity kExpected{1, 2, 3};

// At most two members and four parents; no filesystem or native API execution.
struct TracedOperations {
    std::string member;
    std::vector<std::string>& trace;
    Identity actual = kExpected;
    bool readable = true;
    int flush_failures = 0;
    bool close_ok = true;
    std::filesystem::path failing_parent;

    TracedOperations(std::string name, std::vector<std::string>& events)
        : member(std::move(name))
        , trace(events) {}

    bool bind(const Identity& expected) {
        trace.push_back("bind-" + member);
        return readable && actual == expected && expected != Identity{};
    }
    bool flush_file() {
        trace.push_back("flush-" + member);
        if (flush_failures > 0) {
            --flush_failures;
            return false;
        }
        return true;
    }
    bool close_file() const {
        trace.push_back("close-" + member);
        return close_ok;
    }
    bool flush_parent(const std::filesystem::path& parent) const {
        trace.push_back(parent.generic_string());
        return parent != failing_parent;
    }
};

template<bool FlushFile>
bool CompleteIndexThenPayload(TracedOperations& index, TracedOperations& payload) {
    const Parents parents{"destination", "temporary", "restored", "retired"};
    if (!Luminumbra::Persistence::detail::CompleteRestoration<FlushFile>(index, kExpected, parents))
        return false;
    return Luminumbra::Persistence::detail::CompleteRestoration<FlushFile>(
        payload, kExpected, parents);
}

TEST(RecoveryDurability, MatchingIndexCompletesBeforePayload) {
    std::vector<std::string> trace;
    TracedOperations index("index", trace), payload("payload", trace);
    ASSERT_TRUE(CompleteIndexThenPayload<true>(index, payload));
    EXPECT_EQ(trace,
              (std::vector<std::string>{"bind-index",
                                        "flush-index",
                                        "close-index",
                                        "bind-payload",
                                        "flush-payload",
                                        "close-payload"}));
    // Each failure must stop before even binding the payload. Close still runs on refusal.
    for (const auto failure : {0, 1, 2}) {
        trace.clear();
        index.readable = failure != 0;
        index.flush_failures = failure == 1 ? 1 : 0;
        index.close_ok = failure != 2;
        EXPECT_FALSE(CompleteIndexThenPayload<true>(index, payload));
        EXPECT_EQ(trace.back(), "close-index");
        EXPECT_EQ(trace.size(), failure == 0 ? 2u : 3u);
    }
}

TEST(RecoveryDurability, FailedFlushIsRetriedForMatchingBytes) {
    std::vector<std::string> trace;
    TracedOperations index("index", trace), payload("payload", trace);
    index.flush_failures = 1;
    EXPECT_FALSE(CompleteIndexThenPayload<true>(index, payload));
    EXPECT_EQ(trace, (std::vector<std::string>{"bind-index", "flush-index", "close-index"}));
    trace.clear();
    // The same recorded identity and matching content still have to flush on retry.
    EXPECT_TRUE(CompleteIndexThenPayload<true>(index, payload));
    EXPECT_EQ(trace,
              (std::vector<std::string>{"bind-index",
                                        "flush-index",
                                        "close-index",
                                        "bind-payload",
                                        "flush-payload",
                                        "close-payload"}));
}

TEST(RecoveryDurability, ForeignMatchingBytesRefuse) {
    std::vector<std::string> trace;
    TracedOperations index("index", trace), payload("payload", trace);
    index.actual = {4, 5, 6};
    // Content equality cannot authorize a foreign object, on either backend.
    EXPECT_FALSE(CompleteIndexThenPayload<true>(index, payload));
    EXPECT_EQ(trace, (std::vector<std::string>{"bind-index", "close-index"}));
    trace.clear();
    EXPECT_FALSE(CompleteIndexThenPayload<false>(index, payload));
    EXPECT_EQ(trace, (std::vector<std::string>{"bind-index", "close-index"}));
}

TEST(RecoveryDurability, PosixRetryCompletesRelevantParents) {
    const Parents parents{"destination", "temporary", "restored", "retired"};
    for (const auto& parent : parents) {
        std::vector<std::string> trace;
        TracedOperations index("index", trace), payload("payload", trace);
        index.failing_parent = parent;
        EXPECT_FALSE(CompleteIndexThenPayload<false>(index, payload));
        EXPECT_EQ(trace.back(), parent.generic_string());
        index.failing_parent.clear();
        trace.clear();
        EXPECT_TRUE(CompleteIndexThenPayload<false>(index, payload));
        EXPECT_EQ(trace,
                  (std::vector<std::string>{"bind-index",
                                            "close-index",
                                            "destination",
                                            "temporary",
                                            "restored",
                                            "retired",
                                            "bind-payload",
                                            "close-payload",
                                            "destination",
                                            "temporary",
                                            "restored",
                                            "retired"}));
    }
}

TEST(RecoveryDurability, EmptyParentsAndInvalidBindings) {
    std::vector<std::string> trace;
    TracedOperations operations("index", trace);
    const Parents parents{};
    EXPECT_FALSE(Luminumbra::Persistence::detail::CompleteRestoration<false>(
        operations, kExpected, parents));
    EXPECT_EQ(trace, (std::vector<std::string>{"bind-index", "close-index"}));
    trace.clear();
    EXPECT_TRUE(
        Luminumbra::Persistence::detail::CompleteRestoration<true>(operations, kExpected, parents));
    EXPECT_EQ(trace, (std::vector<std::string>{"bind-index", "flush-index", "close-index"}));
    trace.clear();
    EXPECT_FALSE(Luminumbra::Persistence::detail::CompleteRestoration<true>(
        operations, Identity{}, parents));
    EXPECT_EQ(trace, (std::vector<std::string>{"bind-index", "close-index"}));
    const Parents destination_only{"destination", {}, {}, {}};
    trace.clear();
    EXPECT_TRUE(Luminumbra::Persistence::detail::CompleteRestoration<false>(
        operations, kExpected, destination_only));
    EXPECT_EQ(trace, (std::vector<std::string>{"bind-index", "close-index", "destination"}));
    for (const auto failure : {0, 1}) {
        trace.clear();
        operations.readable = failure != 0;
        operations.close_ok = failure != 1;
        EXPECT_FALSE(Luminumbra::Persistence::detail::CompleteRestoration<false>(
            operations, kExpected, destination_only));
        EXPECT_EQ(trace, (std::vector<std::string>{"bind-index", "close-index"}));
    }
}

using Luminumbra::Persistence::WorldSaveService;

class NativeRecoveryDurability : public ::testing::Test {
protected:
    void SetUp() override {
        root = Luminumbra::TestSupport::CreateUniqueTempDirectory("recovery_");
        // Four private parents and one tiny member bound the native filesystem work.
        for (std::size_t index = 0; index < parents.size(); ++index) {
            parents[index] = root / std::to_string(index);
            ASSERT_TRUE(std::filesystem::create_directory(parents[index]));
        }
        ASSERT_TRUE(WorldSaveService::prepare_restoration(
            parents.front() / "member", "restored bytes", member, identity, &errors));
        ASSERT_NE(identity, Identity{});
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root, error);
        EXPECT_FALSE(error);
    }

    std::filesystem::path root;
    std::filesystem::path member;
    Parents parents{};
    Identity identity{};
    std::vector<std::string> errors;
};

TEST_F(NativeRecoveryDurability, MatchingMemberCompletesTwiceAndReleasesNativeHandle) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        ASSERT_TRUE(WorldSaveService::complete_restoration(member, identity, parents, &errors));
        Identity observed{};
        bool missing = false;
        ASSERT_TRUE(WorldSaveService::artifact_identity(member, observed, missing));
        EXPECT_FALSE(missing);
        EXPECT_EQ(observed, identity);
        EXPECT_EQ(std::filesystem::file_size(member), 14u);
    }
    EXPECT_TRUE(errors.empty());
    // Windows refuses this rename if completion leaked its non-delete-sharing handle.
    const auto renamed = parents.front() / "completed";
    std::filesystem::rename(member, renamed);
    EXPECT_TRUE(std::filesystem::remove(renamed));
}

TEST_F(NativeRecoveryDurability, InvalidIdentityAndMissingOrNonregularMemberRefuse) {
    auto foreign = identity;
    foreign.back() ^= 1;
    for (const auto& expected : {Identity{}, foreign}) {
        EXPECT_FALSE(WorldSaveService::complete_restoration(member, expected, parents, &errors));
    }
    EXPECT_FALSE(
        WorldSaveService::complete_restoration(root / "absent", identity, parents, &errors));
    EXPECT_FALSE(WorldSaveService::complete_restoration(root, identity, parents, &errors));
    ASSERT_TRUE(WorldSaveService::complete_restoration(member, identity, parents, &errors));
    EXPECT_TRUE(std::filesystem::remove(member));
}

#if defined(_WIN32)
TEST_F(NativeRecoveryDurability, DeniedWritableBindingRefusesAndRetriesAfterReaderCloses) {
    const HANDLE reader = ::CreateFileW(member.c_str(),
                                        GENERIC_READ,
                                        FILE_SHARE_READ,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_ATTRIBUTE_NORMAL,
                                        nullptr);
    ASSERT_NE(reader, INVALID_HANDLE_VALUE);
    const bool completed =
        WorldSaveService::complete_restoration(member, identity, parents, &errors);
    EXPECT_NE(::CloseHandle(reader), 0);
    EXPECT_FALSE(completed);
    ASSERT_TRUE(WorldSaveService::complete_restoration(member, identity, parents, &errors));
    EXPECT_TRUE(std::filesystem::remove(member));
}
#else
TEST_F(NativeRecoveryDurability, EveryNativeParentBarrierRefusesMissingOrRegularParentsAndRetries) {
    for (std::size_t index = 0; index < parents.size(); ++index) {
        auto invalid = parents;
        invalid[index] = root / "absent";
        EXPECT_FALSE(WorldSaveService::complete_restoration(member, identity, invalid, &errors));
        invalid[index] = member;
        EXPECT_FALSE(WorldSaveService::complete_restoration(member, identity, invalid, &errors));
        ASSERT_TRUE(WorldSaveService::complete_restoration(member, identity, parents, &errors));
    }
    const Parents empty{};
    EXPECT_FALSE(WorldSaveService::complete_restoration(member, identity, empty, &errors));
    const Parents destination_only{parents.front(), {}, {}, {}};
    EXPECT_TRUE(
        WorldSaveService::complete_restoration(member, identity, destination_only, &errors));
}
#endif

} // namespace
