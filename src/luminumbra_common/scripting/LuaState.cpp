#include "LuaState.h"

#include "LuaApiManifest.h"

namespace Luminumbra::scripting {

LuaState::LuaState() {
}

LuaState::~LuaState() {
}

const LuaApiManifest& LuaState::api_manifest() {
    return GetLuaApiManifest();
}

} // namespace Luminumbra::scripting
