#pragma once
// ma::core - JSON (de)serialization shared by CLI, the analyzer-worker process
// and the GUI bridge. Keeping one canonical wire format here means the GUI can
// rebuild an AnalysisResult/Diagnostic from the worker process verbatim, and the
// CLI emits the exact same shape for golden/regression diffs.
//
// No third-party JSON library is used: a small recursive-descent parser lives in
// the .cpp and both the writer and reader are built on it. The format is fully
// under our control (both ends are in this repo), so the parser only needs to
// handle the structures this module itself emits.
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "ma/core/Analyzer.hpp"
#include "ma/core/MediaInfo.hpp"
#include "ma/core/Result.hpp"

namespace ma::core {

// Serialize a result + diagnostic to a single JSON object. When `type` is
// non-empty it is added as a top-level "type" field (used by the IPC envelope
// so the GUI can distinguish progress lines from result lines). When
// `withFrames` is false the "frames" array is omitted (large transfers / pure
// progress reporting). `ffmpegVersion` is an optional trailing field.
// `frameLimit` caps how many frame rows are serialized (0 = all); the CLI's
// --frames=N uses it so the JSON matches the human table exactly.
// The object always carries top-level "notes" (diagnostic.hint) and, on
// failure, an "error" alias {code,stage,native}; the canonical fields stay
// under "diagnostic". These aliases keep the documented CLI --json contract
// (tests/smoke_cli_json.py) stable.
std::string toJson(const AnalysisResult& r, const Diagnostic& d, bool withFrames,
                   const std::string& type = {},
                   const std::string& ffmpegVersion = {},
                   int64_t frameLimit = 0);

// Parse the object produced by toJson() (tolerant of the extra "type" field and
// of missing/empty sections). Returns false on a hard parse failure; callers
// should treat that as an internal error rather than a media failure.
bool fromJson(const std::string& json, AnalysisResult& r, Diagnostic& d);

// Serialize/parse the AnalysisOptions subset that the worker understands.
std::string toOptionsJson(const AnalysisOptions& o);
bool fromOptionsJson(const std::string& json, AnalysisOptions& o);

// Wrap a string as a JSON string literal ("...") with proper escaping. Used by
// callers that build request envelopes by hand (the GUI builds the analyze
// request from the QVariantMap it already holds).
std::string jsonQuote(const std::string& s);

// Lightweight top-level field extractors for the IPC envelope ({"type":...}).
std::string jsonFieldString(const std::string& json, const std::string& key);
long long jsonFieldInt(const std::string& json, const std::string& key);
double jsonFieldDouble(const std::string& json, const std::string& key);

// Parse a worker request envelope:
//   {"type":"analyze","input":"...","options":{...}}
// Returns false if the envelope is malformed.
bool parseAnalyzeRequest(const std::string& requestJson, std::string& input,
                         AnalysisOptions& opts);

} // namespace ma::core
