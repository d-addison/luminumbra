#include "Log.h"
#include <spdlog/sinks/basic_file_sink.h>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <vector>

std::shared_ptr<spdlog::logger> Log::s_CoreLogger;

namespace {

std::mutex& init_mutex() {
    static std::mutex mu;
    return mu;
}

std::shared_ptr<spdlog::logger> create_default_logger() {
    // spdlog::get returns the existing logger if a previous Init registered it,
    // avoiding the "logger with name LUMINUMBRA already exists" re-registration throw.
    if (auto existing = spdlog::get("LUMINUMBRA")) {
        return existing;
    }

    std::vector<spdlog::sink_ptr> sinks;
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    console->set_pattern("%^[%T] %n: %v%$");
    sinks.push_back(console);

    // Always-on PERSISTENT file sink: the session log survives even when the game is
    // launched by double-click (no console attached) and after a crash. Truncated each
    // run so logs/luminumbra.log is always the CURRENT session — the one that crashed —
    // for post-mortem diagnosis. Console-only fallback if the file can't be opened.
    try {
        std::error_code ec;
        std::filesystem::create_directories("logs", ec);
        auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/luminumbra.log", /*truncate*/ true);
        file->set_pattern("[%Y-%m-%d %T.%e] [%-8l] %v");
        sinks.push_back(file);
    } catch (...) {
    }

    auto logger = std::make_shared<spdlog::logger>("LUMINUMBRA", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::trace);
    // Crash-safety: flush warn/error immediately, and flush everything at least once a
    // second, so the file holds the final breadcrumbs even if the process dies abruptly.
    logger->flush_on(spdlog::level::warn);
    spdlog::register_logger(logger);
    spdlog::flush_every(std::chrono::seconds(1));
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
        // Lazy fallback for callers that bypass Init (test binaries via
        // gtest_main, ad-hoc tooling). Same configuration as Init so the
        // log output is consistent regardless of how the logger came up.
        std::lock_guard<std::mutex> lock(init_mutex());
        if (!s_CoreLogger) {
            s_CoreLogger = create_default_logger();
        }
    }
    return s_CoreLogger;
}
