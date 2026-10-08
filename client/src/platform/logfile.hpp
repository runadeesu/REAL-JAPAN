#pragma once
// The game's log, also written to a file (<userDir>/log.txt; the previous run's is kept as
// log.prev.txt) so a problem on someone's PC can be looked at afterwards. The file gets every
// warning and error and the game's own messages ("RJ: ..."); raylib's routine messages only at
// start-up. Thread-safe (workers log too).

#include <filesystem>

namespace rjc {

// console_level: what still goes to the console (raylib TraceLogLevel); call before InitWindow.
void installLogFile(const std::filesystem::path& path, int console_level);
const std::filesystem::path& logFilePath();

}  // namespace rjc

namespace rjc {

// Freeze watchdog: the main loop calls heartbeat() every frame and names what it is doing with
// phase("..."); when no heartbeat comes for 8 s a watcher thread writes the last phase to the log
// (and again every 30 s while it lasts), so a hang on someone's PC says where it hangs.
void startWatchdog();
void stopWatchdog();
void heartbeat();
void phase(const char* what);  // (a string literal: kept by pointer)

}  // namespace rjc
