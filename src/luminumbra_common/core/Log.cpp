#include "Log.h"
#include <mutex>

std::shared_ptr<spdlog::logger> Log::s_CoreLogger;

namespace {

std::mutex& init_mutex() {
    static std::mutex mu;
    return mu;
}

std::shared_ptr<spdlog::logger> create_default_logger() {
    spdlog::set_pattern("%^[%T] %n: %v%$");
    // spdlog::get returns existing logger if a previous Init() registered it,
    // avoiding the "logger with name LUMINUMBRA already exists" exception
    // that spdlog::stdout_color_mt throws on re-registration.
    auto logger = spdlog::get("LUMINUMBRA");
    if (!logger) {
        logger = spdlog::stdout_color_mt("LUMINUMBRA");
    }
    logger->set_level(spdlog::level::trace);
    return logger;
}

} // namespace

void Log::Init() {
    std::lock_guard<std::mutex> lock(init_mutex());
    if (!s_CoreLogger) {
        s_CoreLogger = create_default_logger();
    }
}

std::shared_ptr<spdlog::logger>& Log::GetCoreLogger() {
    if (!s_CoreLogger) {
        // Lazy fallback for callers that bypass Init() (test binaries via
        // gtest_main, ad-hoc tooling). Same configuration as Init() so the
        // log output is consistent regardless of how the logger came up.
        std::lock_guard<std::mutex> lock(init_mutex());
        if (!s_CoreLogger) {
            s_CoreLogger = create_default_logger();
        }
    }
    return s_CoreLogger;
}
