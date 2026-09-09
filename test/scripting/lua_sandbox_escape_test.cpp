// Exercise the actual Lua host. No dangerous operation is invoked by these tests:
// each negative probe first proves its binding is nil, then checks the call error.
// This does not qualify execution budgets or isolation between chunks.
#include "gtest/gtest.h"

#include "scripting/LuaApiManifest.h"
#include "scripting/LuaState.h"

#include <string>
#include <vector>

namespace {
using namespace Luminumbra::scripting;

struct UnavailableBinding {
    const char* module;
    const char* name;
};

std::string Qualified(const std::string& module, const std::string& name) {
    return module.empty() ? name : module + "." + name;
}

void ExpectUnavailable(const UnavailableBinding& binding) {
    const LuaState lua;
    const auto qualified = Qualified(binding.module, binding.name);
    SCOPED_TRACE(qualified);
    const auto condition =
        std::string(binding.module).empty()
            ? qualified + " == nil"
            : std::string(binding.module) + " == nil or " + qualified + " == nil";
    const auto availability = lua.Evaluate("return (" + condition + ") and 1 or 0");
    ASSERT_EQ(availability.status, LuaEvaluationStatus::Succeeded) << availability.diagnostic;
    // Abort this probe before any call if the binding unexpectedly exists.
    ASSERT_EQ(availability.number, 1.0);
    const auto call = lua.Evaluate(qualified + "()");
    EXPECT_EQ(call.status, LuaEvaluationStatus::RuntimeError);
    EXPECT_FALSE(call.number.has_value());
    EXPECT_FALSE(call.diagnostic.empty());
}

TEST(LuaSandboxEscape, StandardLibraryAndLoaderBindingsAreUnavailable) {
    for (const auto& binding : std::vector<UnavailableBinding>{
             {"io", "open"},
             {"io", "lines"},
             {"io", "popen"},
             {"os", "remove"},
             {"os", "rename"},
             {"os", "tmpname"},
             {"os", "execute"},
             {"os", "exit"},
             {"os", "getenv"},
             {"os", "time"},
             {"os", "clock"},
             {"math", "random"},
             {"package", "loadlib"},
             {"package", "require"},
             {"debug", "getinfo"},
             {"debug", "getupvalue"},
             {"debug", "setupvalue"},
             {"debug", "sethook"},
             {"debug", "getregistry"},
             {"socket", "connect"},
             {"http", "request"},
             {"ffi", "load"},
             {"", "load"},
             {"", "loadstring"},
             {"", "loadfile"},
             {"", "dofile"},
             {"", "require"},
             {"", "rawget"},
             {"", "rawset"},
             {"", "setmetatable"},
             {"", "getmetatable"},
             {"", "collectgarbage"},
             {"", "_G"},
             {"_ENV", "io"},
         }) {
        ExpectUnavailable(binding);
    }
}

TEST(LuaSandboxEscape, ProposedGameplayOperationsAreNotInstalled) {
    for (const auto& binding : std::vector<UnavailableBinding>{
             {"core", "log"},
             {"core", "version"},
             {"entity", "spawn"},
             {"entity", "destroy"},
             {"simulation", "emit_event"},
             {"simulation", "subscribe"},
             {"time", "delta_seconds"},
             {"world", "get_block"},
             {"world", "set_block"},
         }) {
        ExpectUnavailable(binding);
    }
}

TEST(LuaSandboxEscape, EveryAdvertisedEntryIsCallable) {
    const LuaState lua;
    const auto& manifest = LuaState::api_manifest();
    ASSERT_TRUE(LuaApiManifestMeetsBaseline(manifest));
    ASSERT_EQ(manifest.entries.size(), 2u);
    for (const auto& entry : manifest.entries) {
        const auto qualified = Qualified(entry.module, entry.name);
        SCOPED_TRACE(qualified);
        const auto result = lua.Evaluate("return " + qualified + "(0, 0, 0)");
        ASSERT_EQ(result.status, LuaEvaluationStatus::Succeeded) << result.diagnostic;
        EXPECT_EQ(result.number, 0.0);
        EXPECT_TRUE(result.diagnostic.empty());
    }
}

TEST(LuaEvaluation, NumericResultRemainsCompatible) {
    const LuaState lua;
    const auto result = lua.Evaluate("return 6 * 7");
    ASSERT_EQ(result.status, LuaEvaluationStatus::Succeeded);
    EXPECT_EQ(result.number, 42.0);
    EXPECT_TRUE(result.diagnostic.empty());
    double value = -1;
    ASSERT_TRUE(lua.EvalNumber("return 6 * 7", value));
    EXPECT_DOUBLE_EQ(value, 42.0);
}

TEST(LuaEvaluation, SuccessfulNonnumericAndEmptyResultsAreNotExecutionErrors) {
    const LuaState lua;
    for (const auto* chunk : {"",
                              "return",
                              "return nil",
                              "return 'text'",
                              "return false",
                              "return {}, 42",
                              "return function() end"}) {
        SCOPED_TRACE(chunk);
        const auto result = lua.Evaluate(chunk);
        EXPECT_EQ(result.status, LuaEvaluationStatus::Succeeded) << result.diagnostic;
        EXPECT_FALSE(result.number.has_value());
        EXPECT_TRUE(result.diagnostic.empty());
        double unchanged = 123;
        EXPECT_FALSE(lua.EvalNumber(chunk, unchanged));
        EXPECT_DOUBLE_EQ(unchanged, 123.0);
    }
}

TEST(LuaEvaluation, SuccessfulChunkWithNoReturnActuallyExecuted) {
    const LuaState lua;
    const auto write = lua.Evaluate("test_marker = 41");
    ASSERT_EQ(write.status, LuaEvaluationStatus::Succeeded);
    EXPECT_FALSE(write.number.has_value());
    const auto read = lua.Evaluate("return test_marker + 1");
    ASSERT_EQ(read.status, LuaEvaluationStatus::Succeeded);
    EXPECT_EQ(read.number, 42.0);
    // An EvalNumber false result alone cannot prove that this write was denied.
    double unchanged = 123;
    EXPECT_FALSE(lua.EvalNumber("test_marker = 7", unchanged));
    EXPECT_DOUBLE_EQ(unchanged, 123.0);
    ASSERT_TRUE(lua.EvalNumber("return test_marker", unchanged));
    EXPECT_DOUBLE_EQ(unchanged, 7.0);
}

TEST(LuaEvaluation, SyntaxAndRuntimeFailuresHaveDistinctDiagnostics) {
    const LuaState lua;
    const auto syntax = lua.Evaluate("return (");
    EXPECT_EQ(syntax.status, LuaEvaluationStatus::SyntaxError);
    EXPECT_FALSE(syntax.number.has_value());
    EXPECT_FALSE(syntax.diagnostic.empty());
    for (const auto* chunk : {"missing_function()", "return nil + 1"}) {
        SCOPED_TRACE(chunk);
        const auto runtime = lua.Evaluate(chunk);
        EXPECT_EQ(runtime.status, LuaEvaluationStatus::RuntimeError);
        EXPECT_FALSE(runtime.number.has_value());
        EXPECT_FALSE(runtime.diagnostic.empty());
        double unchanged = 123;
        EXPECT_FALSE(lua.EvalNumber(chunk, unchanged));
        EXPECT_DOUBLE_EQ(unchanged, 123.0);
    }
    // Errors leave the interpreter usable; this is not a state-rollback claim.
    EXPECT_EQ(lua.Evaluate("return 1").status, LuaEvaluationStatus::Succeeded);
}
} // namespace
