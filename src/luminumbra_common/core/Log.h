#pragma once
#include <memory>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

class Log {
public:
    /// Explicit one-time initializer. Optional: GetCoreLogger() lazy-inits
    /// to the same configuration on first use if Init() hasn't been called.
    /// Callers in main_client/main_server still call Init() explicitly for
    /// clarity and to pin the pattern early; tests and isolated tools don't
    /// have to.
    static void Init();

    /// Lazy accessor: returns a valid logger even if Init() was never called
    /// (test binaries that use gtest_main, ad-hoc tooling, etc.). Calling this
    /// from a hot path is the same cost as the pre-lazy-init version once the
    /// logger is set.
    static std::shared_ptr<spdlog::logger>& GetCoreLogger();

private:
    static std::shared_ptr<spdlog::logger> s_CoreLogger;
};

// Core log macros
#define LUMINUMBRA_CORE_TRACE(...) ::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define LUMINUMBRA_CORE_INFO(...) ::Log::GetCoreLogger()->info(__VA_ARGS__)
#define LUMINUMBRA_CORE_WARN(...) ::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define LUMINUMBRA_CORE_ERROR(...) ::Log::GetCoreLogger()->error(__VA_ARGS__)
#define LUMINUMBRA_CORE_CRITICAL(...) ::Log::GetCoreLogger()->critical(__VA_ARGS__)
