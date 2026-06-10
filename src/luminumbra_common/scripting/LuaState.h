#pragma once

namespace Luminumbra::scripting {

struct LuaApiManifest;

class LuaState {
public:
    LuaState();
    ~LuaState();

    static const LuaApiManifest& api_manifest();
};

} // namespace Luminumbra::scripting
