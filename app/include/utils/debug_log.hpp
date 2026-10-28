#pragma once

#include <string>

// Dev-only accumulating error log. Mirrors every brls::Logger warning/error
// (plus uncaught C++ exceptions) into a plain-text file on the SD card so
// problems that only reproduce on real hardware -- and that force a full app
// restart to recover from, wiping the in-memory log -- can still be pulled
// off the console and handed over for diagnosis.
//
// REMOVE-BEFORE-RELEASE: this whole feature is gated by STREAMFIN_DEBUG_LOG
// (see CMakeLists.txt). Flip that OFF (or delete the define) before shipping
// a public build; DebugLog::init() becomes a no-op and nothing is written.
namespace DebugLog {

// Opens/creates configDir()/debug.log in append mode and starts mirroring
// brls::Logger events into it. Call once, as early as possible in main().
void init();

// Absolute path to the log file, for surfacing in the UI (e.g. Settings).
std::string path();

}  // namespace DebugLog
