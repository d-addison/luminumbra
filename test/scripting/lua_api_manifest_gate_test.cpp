#include "scripting/LuaApiManifest.h"

#include "gtest/gtest.h"

#include <string>

namespace {
using namespace Luminumbra::scripting;

TEST(LuaApiManifest, DescribesOnlyTheInstalledSamplerAndAlias) {
    const auto& manifest = GetLuaApiManifest();
    ASSERT_TRUE(LuaApiManifestMeetsBaseline(manifest));
    ASSERT_EQ(manifest.entries.size(), 2u);
    EXPECT_EQ(manifest.manifest_version, "1.1.0");
    EXPECT_EQ(manifest.entries[0].module, "");
    EXPECT_EQ(manifest.entries[1].module, "world");
    for (const auto& entry : manifest.entries) {
        EXPECT_EQ(entry.name, "sample_energy_field");
        EXPECT_EQ(entry.kind, "function");
    }
    const auto json = SerializeLuaApiManifestJson(manifest);
    EXPECT_NE(json.find("luminumbra.scripting.lua_api_manifest.v1"), std::string::npos);
    EXPECT_NE(json.find("module_then_name"), std::string::npos);
    EXPECT_NE(json.find("\"module\": \"\""), std::string::npos);
    EXPECT_NE(json.find("world.sample_energy_field"), std::string::npos);
    for (const auto* absent :
         {"set_block", "get_block", "emit_event", "subscribe", "spawn", "destroy"}) {
        EXPECT_EQ(json.find(absent), std::string::npos) << absent;
    }
}

TEST(LuaApiManifest, RejectsMissingDuplicatedUnimplementedAndMalformedEntries) {
    auto manifest = GetLuaApiManifest();
    manifest.entries.pop_back();
    EXPECT_FALSE(LuaApiManifestMeetsBaseline(manifest));
    manifest = GetLuaApiManifest();
    manifest.entries[1] = manifest.entries[0];
    EXPECT_FALSE(LuaApiManifestMeetsBaseline(manifest));
    manifest = GetLuaApiManifest();
    manifest.entries.push_back({"world", "set_block", "function", "world.set_block()", "Unbound"});
    EXPECT_FALSE(LuaApiManifestMeetsBaseline(manifest));
    manifest = GetLuaApiManifest();
    manifest.entries[1].signature.clear();
    EXPECT_FALSE(LuaApiManifestMeetsBaseline(manifest));
    manifest = GetLuaApiManifest();
    manifest.manifest_version = "1.0.0";
    EXPECT_FALSE(LuaApiManifestMeetsBaseline(manifest));
}
} // namespace
