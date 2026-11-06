#pragma once
// ma::core - URL normalization.
// Fixes the exact root causes measured in the legacy app:
//   file:///C:/x  -> file:C:/x   (FFmpeg's file protocol on Windows rejects the
//                                 three-slash drive form with ENOENT)
//   pre-filled "file://path" text concatenated with a pasted URL
//   backslashes / quotes / surrounding whitespace
#include <string>

namespace ma {

struct NormalizedUrl {
    std::string original;
    std::string url;      // final string handed to FFmpeg
    std::string scheme;   // lowercased; "file", "http", "rtsp", ... or empty
    bool isLocal = false;
    bool changed = false;
    std::string notes;    // ';'-joined list of fixes applied
};

// Classify and normalize user input. Never throws; unknown/odd input is passed
// through unchanged and flagged in `notes` so diagnostics stay truthful.
NormalizedUrl normalizeUrl(std::string input);

} // namespace ma
