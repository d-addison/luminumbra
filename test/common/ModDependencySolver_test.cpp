#include "luminumbra_common/mods/ModDependencySolver.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace luminumbra::mods {
namespace {

nlohmann::json Document(std::string_view id = "test.base") {
    return {{"schema", "luminumbra.mod.manifest.v1"},
            {"id", id},
            {"version", "1.2.3"},
            {"tier", "sim_data"}};
}

ModPackage Package(std::string_view id) {
    ModPackage result;
    result.manifest.id = id;
    result.manifest.version = "1.2.3";
    return result;
}

std::vector<std::string> OrderedIds(const std::vector<ModPackage>& packages,
                                    const ModSolution& solution) {
    std::vector<std::string> result;
    for (const auto index : solution.order) {
        result.push_back(packages.at(index).manifest.id);
    }
    return result;
}

void ExpectRefusal(const std::vector<ModPackage>& packages, std::string_view reason) {
    ModSolution result{{42}, 123};
    std::string error;
    EXPECT_FALSE(ModDependencySolver::Solve(packages, result, error));
    EXPECT_NE(error.find(reason), std::string::npos) << error;
    EXPECT_EQ(result.order, (std::vector<std::size_t>{42}));
    EXPECT_EQ(result.identity, 123U);
}

TEST(ModManifestTest, ParsesSchemaSubsetAndAllResidencyTiers) {
    const std::array tiers{std::pair{"client_cosmetic", core::ResidencyClass::Render},
                           std::pair{"client_native", core::ResidencyClass::Render},
                           std::pair{"sim_data", core::ResidencyClass::Sim},
                           std::pair{"sim_script", core::ResidencyClass::Sim},
                           std::pair{"sim_native", core::ResidencyClass::Sim}};
    for (const auto& [tier, residency] : tiers) {
        auto document = Document();
        document["tier"] = tier;
        document["dependencies"] = {
            {{"id", "test.required"}, {"version", "^0.4.0"}},
            {{"id", "test.optional"}, {"version", ">=1.0.0 <2.0.0"}, {"optional", true}}};
        document["incompatible_with"] = {{{"id", "test.conflict"}, {"version", "*"}}};
        document["load_order"] = {{"after", {"test.after"}}, {"before", {"test.before"}}};
        ModManifest manifest;
        std::string error = "old error";
        ASSERT_TRUE(ModManifest::Parse(document.dump(), manifest, error)) << error;
        EXPECT_TRUE(error.empty());
        EXPECT_EQ(manifest.id, "test.base");
        EXPECT_EQ(manifest.version, "1.2.3");
        EXPECT_EQ(manifest.residency(), residency);
        ASSERT_EQ(manifest.dependencies.size(), 2U);
        EXPECT_FALSE(manifest.dependencies[0].optional);
        EXPECT_TRUE(manifest.dependencies[1].optional);
        ASSERT_EQ(manifest.incompatible_with.size(), 1U);
        EXPECT_EQ(manifest.incompatible_with[0].id, "test.conflict");
        EXPECT_EQ(manifest.load_after, (std::vector<std::string>{"test.after"}));
        EXPECT_EQ(manifest.load_before, (std::vector<std::string>{"test.before"}));
    }
}

TEST(ModManifestTest, RejectsInvalidFieldsAndPreservesOutput) {
    std::vector<nlohmann::json> documents{nullptr, 42, nlohmann::json::array(), Document()};
    documents.back().erase("schema");
    for (const auto* key : {"id", "version", "tier"}) {
        auto missing = Document();
        missing.erase(key);
        documents.push_back(missing);
        auto wrong_type = Document();
        wrong_type[key] = 12;
        documents.push_back(wrong_type);
    }
    for (const auto* id : {"base",
                           "Test.base",
                           "test..base",
                           ".test.base",
                           "test.base.",
                           "test/base",
                           "test.\xC3\xA9",
                           "test.a-b"}) {
        auto document = Document(id);
        documents.push_back(document);
    }
    for (const auto* version : {"1",
                                "1.2",
                                "01.2.3",
                                "1.2.3.4",
                                "1.2.3-01",
                                "1.2.3+",
                                "1.2.3-",
                                "18446744073709551616.0.0"}) {
        auto document = Document();
        document["version"] = version;
        documents.push_back(document);
    }
    for (const auto& [key, value] :
         std::array{std::pair{"schema", nlohmann::json("unknown")},
                    std::pair{"tier", nlohmann::json("unknown")},
                    std::pair{"dependencies", nlohmann::json(nullptr)},
                    std::pair{"incompatible_with", nlohmann::json::object()},
                    std::pair{"load_order", nlohmann::json::array()}}) {
        auto document = Document();
        document[key] = value;
        documents.push_back(document);
    }
    for (const auto* key : {"dependencies", "incompatible_with"}) {
        for (const auto& entry : std::vector<nlohmann::json>{
                 nullptr,
                 {{"id", "test.dep"}},
                 {{"id", "test.dep"}, {"version", "1.2"}},
                 {{"id", "test.dep"}, {"version", "^1.2.3"}, {"optional", 1}}}) {
            if (std::string_view(key) == "incompatible_with" && entry.is_object() &&
                entry.contains("optional")) {
                continue;
            }
            auto document = Document();
            document[key] = nlohmann::json::array({entry});
            documents.push_back(document);
        }
    }
    for (const auto* key : {"after", "before"}) {
        for (const auto& value : std::vector<nlohmann::json>{
                 nullptr, "test.dep", {1}, {"invalid"}, {"test.dep", "test.dep"}}) {
            auto document = Document();
            document["load_order"] = {{key, value}};
            documents.push_back(document);
        }
    }
    for (const auto& document : documents) {
        auto result = Package("test.unchanged").manifest;
        std::string error;
        EXPECT_FALSE(ModManifest::Parse(document.dump(), result, error)) << document;
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(result.id, "test.unchanged");
    }
}

TEST(ModManifestTest, RejectsMalformedDuplicateAndDeepJson) {
    const std::vector<std::string> inputs{
        "",
        "{",
        Document().dump() + " trailing",
        R"({"schema":"luminumbra.mod.manifest.v1","id":"test.a","id":"test.b","version":"1.0.0","tier":"sim_data"})",
        std::string(18, '[') + "0" + std::string(18, ']'),
        std::string("{\"unknown\":\"") + static_cast<char>(0xff) + "\"}"};
    for (const auto& input : inputs) {
        ModManifest result;
        std::string error;
        EXPECT_FALSE(ModManifest::Parse(input, result, error));
        EXPECT_FALSE(error.empty());
    }
}

TEST(ModManifestTest, EnforcesByteIdAndRelationBounds) {
    ModManifest manifest;
    std::string error;
    auto text = Document().dump();
    text.resize(kMaxManifestBytes, ' ');
    ASSERT_TRUE(ModManifest::Parse(text, manifest, error)) << error;
    text += ' ';
    EXPECT_FALSE(ModManifest::Parse(text, manifest, error));
    auto document = Document("a." + std::string(126, 'a'));
    ASSERT_TRUE(ModManifest::Parse(document.dump(), manifest, error));
    document["id"] = "a." + std::string(127, 'a');
    EXPECT_FALSE(ModManifest::Parse(document.dump(), manifest, error));
    for (const auto* key : {"dependencies", "incompatible_with", "after", "before"}) {
        auto bounded = Document();
        nlohmann::json entries = nlohmann::json::array();
        for (std::size_t index = 0; index < kMaxModRelations; ++index) {
            const auto id = "test.mod" + std::to_string(index);
            if (std::string_view(key) == "after" || std::string_view(key) == "before") {
                entries.push_back(id);
            } else {
                entries.push_back({{"id", id}, {"version", "*"}});
            }
        }
        const bool hint = std::string_view(key) == "after" || std::string_view(key) == "before";
        if (hint) {
            bounded["load_order"][key] = entries;
        } else {
            bounded[key] = entries;
        }
        ASSERT_TRUE(ModManifest::Parse(bounded.dump(), manifest, error)) << error;
        entries.push_back(entries[0]);
        if (hint) {
            bounded["load_order"][key] = entries;
        } else {
            bounded[key] = entries;
        }
        EXPECT_FALSE(ModManifest::Parse(bounded.dump(), manifest, error));
    }
}

TEST(ModVersionTest, RangeAndPrereleaseBoundaries) {
    struct Case {
        std::string_view version;
        std::string_view range;
        bool matches;
    };
    const std::array cases{
        Case{"1.2.3", "*", true},
        Case{"1.2.3+build.07", "=1.2.3+other", true},
        Case{"1.2.3", ">=1.0.0 <2.0.0", true},
        Case{"2.0.0", ">=1.0.0 <2.0.0", false},
        Case{"1.2.3", ">1.2.3", false},
        Case{"1.2.3", "<=1.2.3", true},
        Case{"1.2.2", ">=1.2.3", false},
        Case{"1.2.4", "<=1.2.3", false},
        Case{"1.2.4", ">1.2.3", true},
        Case{"1.2.2", "<1.2.3", true},
        Case{"1.9.9", "^1.2.3", true},
        Case{"2.0.0", "^1.2.3", false},
        Case{"1.2.2", "^1.2.3", false},
        Case{"0.4.9", "^0.4.0", true},
        Case{"0.5.0", "^0.4.0", false},
        Case{"0.0.3", "^0.0.3", true},
        Case{"0.0.4", "^0.0.3", false},
        Case{"1.2.9", "~1.2.3", true},
        Case{"1.3.0", "~1.2.3", false},
        Case{"1.2.2", "~1.2.3", false},
        Case{"1.2.3-alpha", "*", false},
        Case{"1.2.3-alpha", ">=1.2.3-alpha <2.0.0", true},
        Case{"1.3.0-alpha", ">=1.2.3-alpha <2.0.0", false},
        Case{"1.2.3-alpha.10", ">1.2.3-alpha.9", true},
        Case{"1.2.3-alpha.2", ">1.2.3-alpha.10", false},
        Case{"1.2.3-alpha.beta", ">1.2.3-alpha.99", true},
        Case{"1.2.3-alpha", "<1.2.3-alpha.1", true},
        Case{"1.2.3", ">1.2.3-rc.1", true},
        Case{"1.2.3-rc.1", "1.2.3", false},
        Case{"1.2.3-alpha", "<1.2.3-alpha.1 || >2.0.0", true},
        Case{"1.2.3", "2.0.0 || 1.2.3", true},
        Case{"1.2.3", " \t1.2.3\n", true},
        Case{"18446744073709551615.0.0", "^18446744073709551615.0.0", true},
        Case{"1.18446744073709551615.9", "~1.18446744073709551615.0", true},
        Case{"1.2.3-alpha.999999999999999999999", ">1.2.3-alpha.999999999999999999998", true}};
    for (const auto& entry : cases) {
        EXPECT_EQ(ModVersionMatches(entry.version, entry.range), entry.matches)
            << entry.version << " " << entry.range;
    }
}

TEST(ModVersionTest, RefusesMalformedVersionsAndRanges) {
    for (const auto* version : {"",
                                "0",
                                "1.2",
                                "1.2.3.4",
                                "1.02.3",
                                "1.2.3-00",
                                "1.2.3-.",
                                "1.2.3+bad_",
                                "1.2.3+one+two",
                                "-1.2.3",
                                "1.2.3 ",
                                "18446744073709551616.0.0"}) {
        EXPECT_FALSE(ModVersionMatches(version, "*")) << version;
    }
    for (const auto* range : {"",
                              " ",
                              "||",
                              "* ||",
                              "|| *",
                              "1.2.3 || bad",
                              "1.2.3 | 2.0.0",
                              "^^1.2.3",
                              "~>=1.2.3",
                              "==1.2.3",
                              "1.2",
                              "1.x",
                              "1.2.3 - 2.0.0",
                              ">= 1.2.3",
                              ">18446744073709551616.0.0"}) {
        EXPECT_FALSE(ModVersionMatches("1.2.3", range)) << range;
    }
    EXPECT_FALSE(ModVersionMatches(std::string(129, '1'), "*"));
    EXPECT_FALSE(ModVersionMatches("1.2.3", std::string(513, ' ')));
}

TEST(ModDependencySolverTest, AllInputPermutationsProduceLexicographicTopologicalOrder) {
    std::vector<ModPackage> original{Package("test.a"),
                                     Package("test.b"),
                                     Package("test.c"),
                                     Package("test.d"),
                                     Package("test.e")};
    original[0].manifest.dependencies = {{"test.c", "^1.0.0", false}};
    original[1].manifest.load_after = {"test.c"};
    original[3].manifest.load_before = {"test.c"};
    original[4].manifest.dependencies = {{"test.b", "*", true}};
    original[4].manifest.load_after = {"test.b", "test.absent"};
    original[4].manifest.load_before = {"test.absent"};
    const std::vector<std::string> expected{"test.d", "test.c", "test.a", "test.b", "test.e"};
    std::array<std::size_t, 5> permutation{};
    std::iota(permutation.begin(), permutation.end(), 0U);
    ModSetIdentity identity = 0;
    std::size_t count = 0;
    do {
        std::vector<ModPackage> packages;
        for (const auto index : permutation) {
            packages.push_back(original[index]);
        }
        ModSolution result;
        std::string error = "old error";
        ASSERT_TRUE(ModDependencySolver::Solve(packages, result, error)) << error;
        EXPECT_TRUE(error.empty());
        EXPECT_EQ(OrderedIds(packages, result), expected);
        if (count == 0) {
            identity = result.identity;
        }
        EXPECT_EQ(result.identity, identity);
        ++count;
    } while (std::next_permutation(permutation.begin(), permutation.end()));
    EXPECT_EQ(count, 120U);
}

TEST(ModDependencySolverTest, MissingAndOutOfRangeRequiredAndOptionalDependencies) {
    auto package = Package("test.user");
    package.manifest.dependencies = {{"test.base", "^2.0.0", false}};
    ExpectRefusal({package}, "requires missing mod test.base");
    ExpectRefusal({package, Package("test.base")}, "requires test.base ^2.0.0; installed 1.2.3");
    package.manifest.dependencies[0].optional = true;
    ModSolution solution;
    std::string error;
    ASSERT_TRUE(ModDependencySolver::Solve(std::vector{package}, solution, error));
    ExpectRefusal({package, Package("test.base")}, "requires test.base");
}

TEST(ModDependencySolverTest, ChecksOneSidedAndVersionedIncompatibilities) {
    auto package = Package("test.user");
    package.manifest.incompatible_with = {{"test.base", "^1.0.0", false}};
    ExpectRefusal({package, Package("test.base")}, "test.user incompatible with test.base");
    ExpectRefusal({Package("test.base"), package}, "test.user incompatible with test.base");
    ModSolution solution;
    std::string error;
    ASSERT_TRUE(ModDependencySolver::Solve(std::vector{package}, solution, error));
    package.manifest.incompatible_with[0].version = "^2.0.0";
    ASSERT_TRUE(
        ModDependencySolver::Solve(std::vector{package, Package("test.base")}, solution, error));
}

TEST(ModDependencySolverTest, RejectsDependencyHintMixedAndSelfCyclesWithPaths) {
    auto first = Package("test.a");
    auto second = Package("test.b");
    first.manifest.dependencies = {{"test.b", "*", false}};
    second.manifest.dependencies = {{"test.a", "*", false}};
    ExpectRefusal({first, second}, "test.a -> test.b -> test.a");
    second.manifest.dependencies.clear();
    second.manifest.load_after = {"test.a"};
    ExpectRefusal({first, second}, "cycle");
    first.manifest.dependencies.clear();
    first.manifest.load_before = {"test.b"};
    second.manifest.load_before = {"test.a"};
    ExpectRefusal({first, second}, "cycle");
    first.manifest.load_before = {"test.a"};
    ExpectRefusal({first}, "test.a -> test.a");
    first.manifest.load_before.clear();
    first.manifest.dependencies = {{"test.a", "*", false}};
    ExpectRefusal({first}, "test.a -> test.a");
}

TEST(ModDependencySolverTest, RevalidatesProgrammaticInputsAndDuplicateIds) {
    ExpectRefusal({Package("test.a"), Package("test.a")}, "duplicate mod id");
    ExpectRefusal({Package("invalid")}, "invalid mod id");
    auto package = Package("test.a");
    package.manifest.tier = static_cast<ModTier>(99);
    ExpectRefusal({package}, "invalid mod tier");
    package = Package("test.a");
    package.manifest.dependencies = {{"test.b", "*", false}, {"test.b", "*", true}};
    ExpectRefusal({package}, "invalid or duplicate mod relation");
    package.manifest.dependencies = {{"test.b", "nonsense", true}};
    ExpectRefusal({package}, "invalid or duplicate mod relation");
    package.manifest.dependencies.clear();
    package.manifest.incompatible_with = {{"test.b", "*", false}, {"test.b", "*", false}};
    ExpectRefusal({package}, "invalid or duplicate mod relation");
    package.manifest.incompatible_with.clear();
    package.manifest.load_after = {"test.b", "test.b"};
    ExpectRefusal({package}, "invalid or duplicate load order");
}

TEST(ModDependencySolverTest, EmptySetAndPackageCountBound) {
    ModSolution solution{{42}, 123};
    std::string error;
    ASSERT_TRUE(ModDependencySolver::Solve({}, solution, error));
    EXPECT_TRUE(solution.order.empty());
    EXPECT_EQ(solution.identity, 14695981039346656037ULL);
    std::vector<ModPackage> packages;
    for (std::size_t index = 0; index < kMaxModCount; ++index) {
        packages.push_back(Package("test.mod" + std::to_string(index)));
    }
    ASSERT_TRUE(ModDependencySolver::Solve(packages, solution, error));
    EXPECT_EQ(solution.order.size(), kMaxModCount);
    packages.push_back(Package("test.overflow"));
    ExpectRefusal(packages, "exceeds 1024");
}

TEST(ModSetIdentityTest, WhitespaceKeyOrderAndInstallationRootAreIrrelevant) {
    auto document = Document();
    document["install_root"] = "C:/Games/Mods/first";
    ModPackage first;
    ModPackage second;
    std::string error;
    ASSERT_TRUE(ModManifest::Parse(document.dump(2), first.manifest, error));
    document["install_root"] = "/games/mods/second";
    ASSERT_TRUE(ModManifest::Parse(document.dump(), second.manifest, error));
    ModPackage reordered;
    ASSERT_TRUE(ModManifest::Parse(
        R"({"tier":"sim_data","version":"1.2.3","id":"test.base","schema":"luminumbra.mod.manifest.v1"})",
        reordered.manifest,
        error));
    ModSolution lhs;
    ModSolution rhs;
    ASSERT_TRUE(ModDependencySolver::Solve(std::vector{first}, lhs, error));
    ASSERT_TRUE(ModDependencySolver::Solve(std::vector{second}, rhs, error));
    EXPECT_EQ(lhs.identity, rhs.identity);
    ASSERT_TRUE(ModDependencySolver::Solve(std::vector{reordered}, rhs, error));
    EXPECT_EQ(lhs.identity, rhs.identity);
}

TEST(ModSetIdentityTest, PinsCanonicalFnvEncodingAndEveryContentByte) {
    auto package = Package("test.base");
    for (std::size_t index = 0; index < package.content_hash.size(); ++index) {
        package.content_hash[index] = static_cast<std::uint8_t>(index);
    }
    ModSolution solution;
    std::string error;
    ASSERT_TRUE(ModDependencySolver::Solve(std::vector{package}, solution, error));
    const auto original = solution.identity;
    EXPECT_EQ(original, 0x6111ebc11315784eULL);
    for (std::size_t index = 0; index < package.content_hash.size(); ++index) {
        auto changed = package;
        changed.content_hash[index] ^= 1;
        ASSERT_TRUE(ModDependencySolver::Solve(std::vector{changed}, solution, error));
        EXPECT_NE(solution.identity, original) << index;
    }
    auto changed = package;
    changed.manifest.id = "test.other";
    ASSERT_TRUE(ModDependencySolver::Solve(std::vector{changed}, solution, error));
    EXPECT_NE(solution.identity, original);
    changed = package;
    changed.manifest.version = "1.2.4";
    ASSERT_TRUE(ModDependencySolver::Solve(std::vector{changed}, solution, error));
    EXPECT_NE(solution.identity, original);
    changed = package;
    changed.manifest.version += "+build";
    ASSERT_TRUE(ModDependencySolver::Solve(std::vector{changed}, solution, error));
    EXPECT_NE(solution.identity, original);
}

TEST(ModSetIdentityTest, SolverOrderAffectsIdentity) {
    std::vector packages{Package("test.a"), Package("test.b")};
    ModSolution first;
    ModSolution second;
    std::string error;
    ASSERT_TRUE(ModDependencySolver::Solve(packages, first, error));
    packages[0].manifest.load_after = {"test.b"};
    ASSERT_TRUE(ModDependencySolver::Solve(packages, second, error));
    EXPECT_NE(first.identity, second.identity);
}

} // namespace
} // namespace luminumbra::mods
