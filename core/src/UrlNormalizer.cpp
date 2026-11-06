// ma::core - URL normalization implementation.
// Windows-specific fixes are verified against FFmpeg's file protocol behavior:
//   file:///C:/x  -> ENOENT;  file:C:/x and bare C:/x -> OK.
#include "ma/core/UrlNormalizer.hpp"

#include <algorithm>
#include <cctype>

namespace ma {
namespace {

void addNote(NormalizedUrl& n, const std::string& note) {
    if (!n.notes.empty()) n.notes += "; ";
    n.notes += note;
}

std::string trim(std::string s) {
    auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool isWindowsDrivePath(const std::string& s) {
    return s.size() >= 3 && std::isalpha(static_cast<unsigned char>(s[0])) &&
           s[1] == ':' && (s[2] == '/' || s[2] == '\\');
}

// Schemes FFmpeg commonly handles. Anything else is passed through but flagged.
const char* kKnownSchemes[] = {"http",  "https", "rtsp",  "rtmp",  "rtmps",
                               "srt",   "udp",   "rtp",   "tcp",   "mms",
                               "mmsh",  "ftp",   "file",  "data",  "concat"};

bool isKnownScheme(const std::string& sch) {
    for (const char* k : kKnownSchemes)
        if (sch == k) return true;
    return false;
}

} // namespace

NormalizedUrl normalizeUrl(std::string input) {
    NormalizedUrl n;
    n.original = input;

    // 1) trim whitespace
    input = trim(std::move(input));
    if (input != n.original) addNote(n, "trimmed whitespace");

    // 2) strip surrounding quotes (drag&drop and copy-paste artifacts)
    while (input.size() >= 2 && ((input.front() == '"' && input.back() == '"') ||
                                 (input.front() == '\'' && input.back() == '\''))) {
        input = input.substr(1, input.size() - 2);
        addNote(n, "stripped quotes");
    }

    // 3) file:// handling - the legacy app's main killer
    if (toLower(input.substr(0, 7)) == "file://") {
        std::string rest = input.substr(7); // after "file://"
        // "file:///C:/x" -> rest "/C:/x";  "file://C:/x" -> rest "C:/x"
        if (!rest.empty() && rest[0] == '/') {
            // "/C:/x" (drive) -> drop the single leading slash; keep UNC "//srv/share" intact
            if (rest.size() >= 2 && rest[1] != '/') {
                rest.erase(0, 1);
                addNote(n, "file:///: dropped extra slash (FFmpeg file protocol on Windows)");
            }
        }
        if (rest.find('\\') != std::string::npos) {
            std::replace(rest.begin(), rest.end(), '\\', '/');
            addNote(n, "backslashes converted to slashes");
        }
        n.scheme = "file";
        n.isLocal = true;
        n.url = "file:" + rest;
        if (n.url != input) n.changed = true;
        return n;
    }

    // 4) bare Windows path "C:\x\y.mp4" or "C:/x/y.mp4" -> plain path, no scheme
    if (isWindowsDrivePath(input)) {
        std::replace(input.begin(), input.end(), '\\', '/');
        n.scheme = "file";
        n.isLocal = true;
        n.url = input;
        n.changed = (n.url != n.original);
        if (n.changed) addNote(n, "Windows path normalized");
        return n;
    }

    // 5) UNC path "\\server\share\x" -> keep as-is with slashes
    if (input.size() >= 2 && input[0] == '\\' && input[1] == '\\') {
        std::replace(input.begin(), input.end(), '\\', '/');
        n.scheme = "file";
        n.isLocal = true;
        n.url = input;
        addNote(n, "UNC path normalized");
        return n;
    }

    // 6) generic scheme://...
    auto pos = input.find("://");
    if (pos != std::string::npos && pos > 0) {
        std::string sch = toLower(input.substr(0, pos));
        bool valid = std::all_of(sch.begin(), sch.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '+' || c == '-' || c == '.';
        });
        if (valid) {
            n.scheme = sch;
            n.url = input;
            if (!isKnownScheme(sch)) addNote(n, "unknown scheme, passed through as-is");
            return n;
        }
    }

    // 7) fallback: treat as local relative path
    std::replace(input.begin(), input.end(), '\\', '/');
    n.scheme = "file";
    n.isLocal = true;
    n.url = input;
    return n;
}

} // namespace ma
