#include "Log.h"

std::shared_ptr<spdlog::logger> Log::s_CoreLogger;

void Log::Init() {
    // Set pattern, create a color-coded console sink, and create the logger
    spdlog::set_pattern("%^[%T] %n: %v%$");
    s_CoreLogger = spdlog::stdout_color_mt("LUMINUMBRA");
    s_CoreLogger->set_level(spdlog::level::trace);
}
