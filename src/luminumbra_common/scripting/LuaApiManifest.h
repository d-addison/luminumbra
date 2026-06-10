#pragma once

#include <string>
#include <vector>

namespace Luminumbra::scripting {

struct LuaApiManifestEntry {
    std::string module;
    std::string name;
    std::string kind;
    std::string signature;
    std::string description;
};

struct LuaApiManifest {
    std::string schema;
    std::string manifest_version;
    std::string deterministic_order;
    std::vector<LuaApiManifestEntry> entries;
};

const LuaApiManifest& GetLuaApiManifest();
std::string SerializeLuaApiManifestJson(const LuaApiManifest& manifest);
bool LuaApiManifestMeetsBaseline(const LuaApiManifest& manifest);

} // namespace Luminumbra::scripting
