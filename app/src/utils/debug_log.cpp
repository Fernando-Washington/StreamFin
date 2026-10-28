#include "utils/debug_log.hpp"

#ifdef STREAMFIN_DEBUG_LOG

#include "utils/config.hpp"

#include <borealis/core/logger.hpp>
#include <fmt/chrono.h>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <sys/stat.h>

namespace {

std::FILE* logFile = nullptr;
std::mutex logFileMtx;

// Session logs accumulate on purpose (that's the point), but a debug build
// left running across many test sessions on an SD card shouldn't grow
// forever. Anything past this is dropped from the *start* of a fresh
// session so the most recent (most relevant) runs are always kept whole.
constexpr long kMaxLogBytes = 8L * 1024 * 1024;

void writeLine(const std::string& line) {
    std::unique_lock<std::mutex> lock(logFileMtx);
    if (!logFile) return;
    std::fwrite(line.data(), 1, line.size(), logFile);
    std::fflush(logFile);  // a hang/forced-restart must not lose what's buffered
}

std::string formatEvent(brls::Logger::TimePoint tp, brls::LogLevel level, const std::string& msg) {
    std::time_t tt = std::chrono::system_clock::to_time_t(tp);
    std::tm tm = *std::localtime(&tt);
    const char* tag = "INFO";
    switch (level) {
        case brls::LogLevel::LOG_ERROR: tag = "ERROR"; break;
        case brls::LogLevel::LOG_WARNING: tag = "WARN"; break;
        case brls::LogLevel::LOG_INFO: tag = "INFO"; break;
        case brls::LogLevel::LOG_DEBUG: tag = "DEBUG"; break;
        case brls::LogLevel::LOG_VERBOSE: tag = "VERBOSE"; break;
    }
    return fmt::format("{:%Y-%m-%d %H:%M:%S} [{}] {}\n", tm, tag, msg);
}

}  // namespace

namespace DebugLog {

std::string path() {
    return AppConfig::instance().configDir() + "/debug.log";
}

void init() {
    std::string dir = AppConfig::instance().configDir();
#ifdef _WIN32
    ::mkdir(dir.c_str());
#else
    ::mkdir(dir.c_str(), 0777);
#endif

    std::string file = path();
    // If the log from previous sessions grew past the cap, start clean
    // instead of appending forever -- keeps it small enough to actually
    // copy off the SD card and send over.
    struct stat st{};
    if (::stat(file.c_str(), &st) == 0 && st.st_size > kMaxLogBytes) {
        std::remove(file.c_str());
    }

    logFile = std::fopen(file.c_str(), "a");
    if (!logFile) {
        brls::Logger::warning("DebugLog: could not open {}", file);
        return;
    }

    std::set_terminate([]() {
        writeLine(fmt::format("=== uncaught exception -> std::terminate ===\n"));
        std::abort();
    });

    brls::Logger::getLogEvent()->subscribe(
        [](brls::Logger::TimePoint tp, brls::LogLevel level, std::string msg) {
            writeLine(formatEvent(tp, level, msg));
        });

    writeLine(fmt::format("\n===== StreamFin session start ({} {}) =====\n",
                           AppVersion::getVersion(), AppVersion::getCommit()));
    brls::Logger::info("DebugLog: writing to {}", file);
}

}  // namespace DebugLog

#else  // !STREAMFIN_DEBUG_LOG

namespace DebugLog {
void init() {}
std::string path() { return {}; }
}  // namespace DebugLog

#endif
