#pragma once
// ma::infra - last-line crash observability. On Windows this installs a
// process-wide unhandled-exception filter that writes a minidump to
// <exedir>/crashes/<app>-<timestamp>.dmp before the process exits, so a
// decoder/FFmpeg fault in the analyzer worker (or anywhere) becomes a
// reproducible artifact instead of a silent disappearance.
//
// It also covers the C-runtime failure modes (purecall, invalid parameter,
// terminate) and SIGSEGV/SIGABRT where available. On non-Windows platforms
// installCrashHandler() is a no-op.
namespace ma::infra {

// Install the crash handler. Safe to call multiple times (idempotent).
// `appName` is used only to label the dump file (e.g. "analyzer-worker").
void installCrashHandler(const char* appName);

// Path of the most recently written dump in THIS process, or "" if none.
// (For the worker, the GUI finds dumps by scanning <exedir>/crashes instead.)
const char* lastCrashDumpPath();

} // namespace ma::infra
