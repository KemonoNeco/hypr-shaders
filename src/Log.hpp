#pragma once
#include <format>
#include <hyprland/src/debug/log/Logger.hpp>

namespace hs {
    using Log::DEBUG;
    using Log::ERR;
    using Log::WARN;

    template <typename... Args>
    void log(Hyprutils::CLI::eLogLevel level, std::format_string<Args...> fmt, Args&&... args) {
        Log::logger->log(level, "[hypr-shaders] {}", std::format(fmt, std::forward<Args>(args)...));
    }
}
