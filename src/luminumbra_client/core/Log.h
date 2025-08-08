#pragma once
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <memory>

class Log {
public:
    static void Init();

    static std::shared_ptr<spdlog::logger>& GetCoreLogger() { return s_CoreLogger; }

private:
    static std::shared_ptr<spdlog::logger> s_CoreLogger;
};

// Core log macros
#define LUMINUMBRA_CORE_TRACE(...)    ::Log::GetCoreLogger()->trace(__VA_ARGS__)
#define LUMINUMBRA_CORE_INFO(...)     ::Log::GetCoreLogger()->info(__VA_ARGS__)
#define LUMINUMBRA_CORE_WARN(...)     ::Log::GetCoreLogger()->warn(__VA_ARGS__)
#define LUMINUMBRA_CORE_ERROR(...)    ::Log::GetCoreLogger()->error(__VA_ARGS__)
#define LUMINUMBRA_CORE_CRITICAL(...) ::Log::GetCoreLogger()->critical(__VA_ARGS__)
