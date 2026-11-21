#include "Log.h"
#include "BoundedConsoleSink.h"
#include "LogDispatcherSink.h"
#include <chrono>
#include <filesystem>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <vector>

namespace {

std::shared_ptr<spdlog::logger> create_default_logger() {
    // spdlog::get returns the existing logger if a previous Init registered it,
    // avoiding the "logger with name LUMINUMBRA already exists" re-registration throw.
    if (auto existing = spdlog::get("LUMINUMBRA")) {
        return existing;
    }

    std::vector<spdlog::sink_ptr> sinks;
    // The choice depends only on the destination. A terminal keeps the synchronous
    // coloured sink. Redirected output (pipe, file) goes through the bounded sink so
    // a stalled reader cannot block logging callers. If the output cannot be
    // duplicated, the bounded sink starts Dead and drops console messages, while the
    // file sink keeps recording them.
    std::shared_ptr<spdlog::sinks::sink> console;
    if (!BoundedConsoleSink::StdoutIsCharacterDevice()) {
        console = std::make_shared<BoundedConsoleSink>(BoundedConsoleSink::MakeStdoutOutput(),
                                                       BoundedConsoleSink::Limits{});
    } else {
        console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    }
    console->set_pattern("%^[%T] %n: %v%$");
    sinks.push_back(console);

    // Always-on PERSISTENT file sink: the session log survives even when the game is
    // launched by double-click (no console attached) and after a crash. Truncated each
    // run so logs/luminumbra.log is always the CURRENT session — the one that crashed —
    // for post-mortem diagnosis. Console-only fallback if the file can't be opened.
    try {
        std::error_code ec;
        std::filesystem::create_directories("logs", ec);
        auto file = std::make_shared<spdlog::sinks::basic_file_sink_mt>("logs/luminumbra.log",
                                                                        /*truncate*/ true);
        file->set_pattern("[%Y-%m-%d %T.%e] [%-8l] %v");
        sinks.push_back(file);
    } catch (...) {}

    sinks.push_back(Log::GetDispatcherSink());

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
    (void)GetCoreLogger();
}

const std::shared_ptr<spdlog::logger>& Log::GetCoreLogger() {
    static const auto logger = create_default_logger();
    return logger;
}

std::shared_ptr<LogDispatcherSink> Log::GetDispatcherSink() {
    static auto dispatcher = std::make_shared<LogDispatcherSink>();
    return dispatcher;
}
