#include "../../src/luminumbra_common/scripting/LuaApiManifest.h"

#include <stdexcept>
#include <string>

namespace {

bool RunLuaApiManifestGate() {
    const auto& manifest = Luminumbra::scripting::GetLuaApiManifest();
    if (!Luminumbra::scripting::LuaApiManifestMeetsBaseline(manifest)) {
        throw std::runtime_error("Lua API manifest does not meet the baseline contract");
    }

    const std::string json = Luminumbra::scripting::SerializeLuaApiManifestJson(manifest);
    for (const char* needle : {
             "luminumbra.scripting.lua_api_manifest.v1",
             "module_then_name",
             "\"module\": \"core\"",
             "\"name\": \"log\"",
             "\"module\": \"world\"",
             "\"name\": \"set_block\"",
         }) {
        if (json.find(needle) == std::string::npos) {
            throw std::runtime_error("Lua API manifest JSON is missing a required token");
        }
    }

    return true;
}

const bool kLuaApiManifestGateResult = RunLuaApiManifestGate();

} // namespace
