#pragma once
// ma::infra - process-wide, crash-safe file logging.
//
// Why this exists: the shipped GUI is a WIN32 subsystem executable, so neither
// stderr nor Qt/QML warnings are visible to a user running the packaged build.
// The analysis kernel is a separate process whose FFmpeg diagnostics disappear
// with it. Every binary (GUI / worker / CLI) therefore opens its OWN log file
// under a shared directory and writes plain UTF-8 lines that survive a crash.
//
// Properties:
//   * one file per process name (media-analyse.log / analyzer-worker.log /
//     media-analyse-cli.log), so concurrent processes never need a named lock;
//   * appended with FILE_APPEND_DATA (+ shared read/write) on Windows: writes
//     are atomic per line and survive std::_Exit()/SEH without an explicit
//     flush or destructor;
//   * size-based rotation, keeping N generations;
//   * directory: MA_LOG_DIR -> <exe dir>/logs -> %LOCALAPPDATA%/MediaAnalyse/
//     logs -> %TEMP%/MediaAnalyse/logs (first writable one wins);
//   * MA_LOG_LEVEL=debug|info|warn|error|off, MA_LOG_MAX_BYTES, MA_LOG_MAX_FILES;
//   * credentials in URLs are masked before anything reaches the file.
//
// This module depends on neither Qt nor FFmpeg. It must be safe to call from an
// exception filter, hence emergencyLog() takes no locks.
#include <sstream>
#include <string>

namespace ma::infra {

enum class Severity { Debug = 0, Info = 1, Warning = 2, Error = 3, Fatal = 4, Off = 5 };

// Open the log file for this process. Idempotent; returns false if no writable
// directory could be found (in which case all logging is a silent no-op).
// Call as early as possible in main(), before anything that can fail.
bool initLogger(const char* appName);

bool loggerEnabled();
Severity logLevel();

// UTF-8 paths; empty when logging is disabled. Safe to expose in the UI so a
// user can find the diagnostics without guessing.
std::string logFilePath();
std::string logDirectory();

// Thread-safe, level-filtered, credential-redacting log write.
void logMessage(Severity sev, const char* component, const std::string& message);

// Crash-path write: no mutex, no allocation in the caller, opens its own
// append handle. Used by CrashHandler from inside the unhandled filter, where
// the logging mutex may already be held by the faulting thread.
void emergencyLog(const char* component, const std::string& message);

// Mask userinfo and sensitive query parameters in a URL-ish string. Plain
// local paths and ordinary messages are returned unchanged.
std::string redact(const std::string& text);

void closeLogger();

}  // namespace ma::infra

// Stream-style macros keep call sites readable and avoid formatting when the
// logger is off: MA_LOGI("bridge", "worker pid=" << pid << " input=" << url)
#define MA_LOG_AT(sev, comp, expr)                                  \
    do {                                                            \
        if (::ma::infra::loggerEnabled()) {                         \
            std::ostringstream _ma_log_ss;                          \
            _ma_log_ss << expr;                                     \
            ::ma::infra::logMessage((sev), (comp), _ma_log_ss.str()); \
        }                                                           \
    } while (false)

#define MA_LOGD(comp, expr) MA_LOG_AT(::ma::infra::Severity::Debug, comp, expr)
#define MA_LOGI(comp, expr) MA_LOG_AT(::ma::infra::Severity::Info, comp, expr)
#define MA_LOGW(comp, expr) MA_LOG_AT(::ma::infra::Severity::Warning, comp, expr)
#define MA_LOGE(comp, expr) MA_LOG_AT(::ma::infra::Severity::Error, comp, expr)
#define MA_LOGF(comp, expr) MA_LOG_AT(::ma::infra::Severity::Fatal, comp, expr)
