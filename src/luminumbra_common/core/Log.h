#pragma once
#include <memory>
#include <spdlog/spdlog.h>

class LogDispatcherSink;

class Log {
public:
    /// Explicit one-time initializer. Optional: GetCoreLogger() lazy-inits
    /// to the same configuration on first use if Init() hasn't been called.
    /// Callers in main_client/main_server still call Init() explicitly for
    /// clarity and to pin the pattern early; tests and isolated tools don't
    /// have to.
    static void Init();
    static std::shared_ptr<LogDispatcherSink> GetDispatcherSink();

    /// Thread-safe lazy accessor, including concurrent calls to Init(). The
    /// owner cannot be replaced; attach temporary sinks through the dispatcher.
    static const std::shared_ptr<spdlog::logger>& GetCoreLogger();
};

// Core log macros
#define LUMINUMBRA_CORE_TRACE(...) ::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define LUMINUMBRA_CORE_INFO(...) ::Log::GetCoreLogger()->info(__VA_ARGS__)
#define LUMINUMBRA_CORE_WARN(...) ::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define LUMINUMBRA_CORE_ERROR(...) ::Log::GetCoreLogger()->error(__VA_ARGS__)
#define LUMINUMBRA_CORE_CRITICAL(...) ::Log::GetCoreLogger()->critical(__VA_ARGS__)
