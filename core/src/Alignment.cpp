// ma::core - HLS timeline + alignment diagnostics. See Alignment.hpp for the
// contract: PDT is an OPTIONAL anchor, never a prerequisite.
#include "ma/core/Alignment.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace ma {
namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}

bool startsWith(const std::string& s, const char* p) {
    return s.rfind(p, 0) == 0;
}

int64_t daysFromCivil(int y, unsigned m, unsigned d) {
    y -= (m <= 2);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

void civilFromDays(int64_t z, int& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t yy = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    yy += (m <= 2);
    y = static_cast<int>(yy);
}

double medianOf(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

void addIssue(AlignmentReport& rep, const char* severity, const char* code,
              const std::string& message, int streamIndex, int segmentIndex,
              double atSeconds) {
    AlignmentIssue i;
    i.severity = severity;
    i.code = code;
    i.message = message;
    i.streamIndex = streamIndex;
    i.segmentIndex = segmentIndex;
    i.atSeconds = atSeconds;
    if (std::string(severity) == "error") ++rep.errorCount;
    else if (std::string(severity) == "warning") ++rep.warningCount;
    rep.issues.push_back(std::move(i));
}

std::string msText(double ms) {
    char b[48];
    std::snprintf(b, sizeof(b), "%+.1f", ms);
    return b;
}

std::string secText(double s) {
    char b[48];
    std::snprintf(b, sizeof(b), "%.3f", s);
    return b;
}

} // namespace

// ------------------------------------------------------------ ISO-8601 UTC ---

std::optional<int64_t> parseIso8601ToEpochMs(const std::string& in) {
    const std::string s = trim(in);
    // YYYY-MM-DDThh:mm:ss(.fff)(Z|+HHMM|+HH:MM|-HHMM)
    if (s.size() < 19 || s[4] != '-' || s[7] != '-' ||
        (s[10] != 'T' && s[10] != 't' && s[10] != ' ') ||
        s[13] != ':' || s[16] != ':')
        return std::nullopt;

    auto num = [&](size_t at, size_t n) -> int {
        int v = 0;
        for (size_t k = 0; k < n; ++k) {
            if (at + k >= s.size() || s[at + k] < '0' || s[at + k] > '9') return -1;
            v = v * 10 + (s[at + k] - '0');
        }
        return v;
    };
    const int y = num(0, 4), mo = num(5, 2), d = num(8, 2);
    const int h = num(11, 2), mi = num(14, 2), se = num(17, 2);
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 ||
        mi < 0 || mi > 59 || se < 0 || se > 60)
        return std::nullopt;

    size_t p = 19;
    int millis = 0;
    if (p < s.size() && (s[p] == '.' || s[p] == ',')) {
        ++p;
        int scale = 100;
        while (p < s.size() && s[p] >= '0' && s[p] <= '9' && scale > 0) {
            millis += (s[p] - '0') * scale;
            scale /= 10;
            ++p;
        }
    }

    int64_t offsetMs = 0; // local - UTC
    if (p < s.size()) {
        const char z = s[p];
        if (z == 'Z' || z == 'z') {
            ++p;
        } else if (z == '+' || z == '-') {
            ++p;
            // accept +0800 / +08:00 / +08
            int oh = num(p, 2);
            if (oh < 0) return std::nullopt;
            p += 2;
            int om = 0;
            if (p < s.size() && s[p] == ':') ++p;
            if (p + 1 < s.size() && s[p] >= '0' && s[p] <= '9') om = num(p, 2);
            offsetMs = (static_cast<int64_t>(oh) * 3600 +
                        static_cast<int64_t>(om) * 60) * 1000;
            if (z == '-') offsetMs = -offsetMs;
        } else {
            // No timezone designator: RFC 8216 requires one, but recordings in
            // the wild sometimes omit it. Treat as UTC rather than dropping the
            // only wall-clock anchor we have.
        }
    }

    const int64_t days = daysFromCivil(y, static_cast<unsigned>(mo),
                                       static_cast<unsigned>(d));
    const int64_t local = (days * 86400 + h * 3600 + mi * 60 + se) * 1000 + millis;
    return local - offsetMs;
}

std::string formatEpochMsUtc(int64_t ms) {
    const int64_t totalSec = ms / 1000;
    const int millis = static_cast<int>(ms % 1000);
    int64_t days = totalSec / 86400;
    int64_t rem = totalSec % 86400;
    if (rem < 0) { rem += 86400; --days; }
    if (millis < 0) return {};
    int y; unsigned mo, d;
    civilFromDays(days, y, mo, d);
    char b[64];
    std::snprintf(b, sizeof(b), "%04d-%02u-%02uT%02lld:%02lld:%02lld.%03dZ",
                  y, mo, d, static_cast<long long>(rem / 3600),
                  static_cast<long long>((rem % 3600) / 60),
                  static_cast<long long>(rem % 60), millis);
    return b;
}

// --------------------------------------------------------------- playlist ---

HlsPlaylist parseHlsPlaylist(const std::string& text, const std::string& source) {
    HlsPlaylist pl;
    pl.source = source;

    double elapsed = 0.0;
    double pendingExtinf = -1.0;
    std::optional<int64_t> pendingPdt;
    bool pendingDiscontinuity = false;
    bool sawHeader = false;

    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t nl = text.find('\n', pos);
        const std::string rawLine = text.substr(pos, nl == std::string::npos
                                                       ? std::string::npos
                                                       : nl - pos);
        const std::string line = trim(rawLine);
        if (nl == std::string::npos) pos = text.size() + 1;
        else pos = nl + 1;

        if (line.empty()) continue;
        if (line[0] == '#') {
            if (startsWith(line, "#EXTM3U")) sawHeader = true;
            else if (startsWith(line, "#EXT-X-STREAM-INF")) pl.isMaster = true;
            else if (startsWith(line, "#EXT-X-TARGETDURATION:"))
                pl.targetDuration = std::atof(line.c_str() + 22);
            else if (startsWith(line, "#EXT-X-MEDIA-SEQUENCE:"))
                pl.mediaSequence = std::atoll(line.c_str() + 22);
            else if (startsWith(line, "#EXTINF:")) {
                const std::string v = line.substr(8);
                pendingExtinf = std::atof(v.c_str());
            } else if (startsWith(line, "#EXT-X-PROGRAM-DATE-TIME:"))
                pendingPdt = parseIso8601ToEpochMs(line.substr(25));
            else if (line == "#EXT-X-DISCONTINUITY")
                pendingDiscontinuity = true;
            else if (startsWith(line, "#EXT-X-ENDLIST")) pl.hasEndList = true;
            // KEY / BYTERANGE / MAP / independent segments are intentionally
            // ignored: they do not change the elapsed-time model.
            continue;
        }

        HlsSegment seg;
        seg.index = static_cast<int>(pl.segments.size());
        seg.mediaSequence = pl.mediaSequence + seg.index;
        seg.uri = line;
        seg.extinfSeconds = pendingExtinf > 0 ? pendingExtinf : 0.0;
        seg.pdtMs = pendingPdt;
        seg.discontinuity = pendingDiscontinuity;
        seg.playlistStartSeconds = elapsed;
        elapsed += seg.extinfSeconds;
        if (pendingPdt) ++pl.pdtCount;
        pl.segments.push_back(std::move(seg));

        pendingExtinf = -1.0;
        pendingPdt.reset();
        pendingDiscontinuity = false;
    }
    (void)sawHeader;
    return pl;
}

// ------------------------------------------------------------- diagnostics --

AlignmentReport computeAlignment(const AnalysisResult& r, const HlsPlaylist& playlist) {
    AlignmentReport rep;
    rep.playlist = playlist;                 // keep demux-measured fields
    const auto& segs = rep.playlist.segments;

    if (segs.empty()) rep.timeline = TimelineKind::PtsOnly;
    else if (rep.playlist.hasPdt()) rep.timeline = TimelineKind::ProgramDateTime;
    else rep.timeline = TimelineKind::PlaylistElapsed;

    switch (rep.timeline) {
    case TimelineKind::ProgramDateTime:
        rep.timelineNote = "以 #EXT-X-PROGRAM-DATE-TIME 为绝对墙上时钟，可与其它录制文件直接比对";
        break;
    case TimelineKind::PlaylistElapsed:
        rep.timelineNote = "无 PDT，以 ΣEXTINF 构造相对时间轴；可查分片/音画问题，跨文件只能相对对齐";
        break;
    case TimelineKind::PtsOnly:
        rep.timelineNote = "未读取到 HLS 清单，仅以包 pts 做流内与流间诊断";
        break;
    }
    rep.crossFileComparable = rep.timeline == TimelineKind::ProgramDateTime;

    // Wall-clock coverage from PDT (milliseconds since epoch).
    if (rep.timeline == TimelineKind::ProgramDateTime) {
        for (const auto& s : segs) {
            if (!s.pdtMs) continue;
            if (!rep.firstWallClockMs || *s.pdtMs < *rep.firstWallClockMs)
                rep.firstWallClockMs = *s.pdtMs;
            const int64_t end = *s.pdtMs +
                                static_cast<int64_t>(s.extinfSeconds * 1000.0);
            if (!rep.lastWallClockMs || end > *rep.lastWallClockMs)
                rep.lastWallClockMs = end;
        }
    }

    // --- per-stream statistics ---------------------------------------------
    double origin = 0.0;
    bool haveOrigin = false;
    for (const auto& f : r.frames) {
        const auto t = f.ptsTime ? f.ptsTime : f.dtsTime;
        if (!t) continue;
        if (!haveOrigin || *t < origin) { origin = *t; haveOrigin = true; }
    }

    std::map<int, std::vector<const FrameRecord*>> byStream;
    for (const auto& f : r.frames)
        if (f.ptsTime || f.dtsTime) byStream[f.streamIndex].push_back(&f);

    int refStream = -1;
    for (const auto& s : r.streams) {
        if (s.kind == StreamKind::Video && byStream.count(s.index)) {
            refStream = s.index;
            break;
        }
    }
    if (refStream < 0 && !byStream.empty()) refStream = byStream.begin()->first;
    rep.referenceStream = refStream;

    double refFirst = 0.0;
    if (refStream >= 0 && byStream.count(refStream)) {
        for (const auto* f : byStream.at(refStream)) {
            const auto t = f->ptsTime ? f->ptsTime : f->dtsTime;
            if (t) { refFirst = *t - origin; break; }
        }
    }

    for (const auto& [idx, rows] : byStream) {
        StreamSyncMeta m;
        m.streamIndex = idx;
        for (const auto& s : r.streams)
            if (s.index == idx) { m.kind = s.kind; m.codec = s.codecName; break; }
        m.packetCount = static_cast<int>(rows.size());

        std::vector<double> times;
        times.reserve(rows.size());
        for (const auto* f : rows) {
            const auto t = f->ptsTime ? f->ptsTime : f->dtsTime;
            if (t) times.push_back(*t - origin);
        }
        if (times.empty()) continue;
        m.firstPtsSeconds = times.front();
        m.lastPtsSeconds = times.back();
        m.startOffsetMs = (times.front() - refFirst) * 1000.0;

        std::vector<double> deltas;
        for (size_t i = 1; i < times.size(); ++i) {
            const double d = times[i] - times[i - 1];
            if (d < -1e-6) ++m.nonMonotonicCount;
            else deltas.push_back(d);
        }
        if (!deltas.empty()) {
            m.maxGapSeconds = *std::max_element(deltas.begin(), deltas.end());
            const double typical = medianOf(deltas);
            // With a playlist, gap attribution happens in the segment checks
            // below (unmarked vs declared discontinuity). Without one, report
            // the raw gap here.
            if (m.maxGapSeconds > 0.5 && typical > 0 &&
                m.maxGapSeconds > 3.0 * typical && segs.empty())
                addIssue(rep, "warning", "stream-gap",
                         "流 #" + std::to_string(idx) + " 出现 " +
                             secText(m.maxGapSeconds) + " s 时间戳空档（典型间隔 " +
                             secText(typical) + " s）",
                         idx, -1, times.front() + m.maxGapSeconds);
        }

        // Clock drift = median(pts increment - declared packet duration) /
        // typical increment. This is robust to gaps (a single 1 s hole does
        // not move the median) and, unlike comparing audio/video first-pts at
        // arbitrary segment boundaries, is not dominated by one-frame jitter.
        std::vector<double> residuals;
        for (size_t i = 1; i < rows.size(); ++i) {
            const auto t0 = rows[i - 1]->ptsTime ? rows[i - 1]->ptsTime
                                                 : rows[i - 1]->dtsTime;
            const auto t1 = rows[i]->ptsTime ? rows[i]->ptsTime : rows[i]->dtsTime;
            if (!t0 || !t1 || !rows[i - 1]->durationTime) continue;
            const double d = *t1 - *t0;
            if (d <= 0.0 || d > 0.5) continue; // skip holes / reordering
            residuals.push_back(d - *rows[i - 1]->durationTime);
        }
        const double typicalDelta = medianOf(deltas);
        if (!residuals.empty() && typicalDelta > 1e-6)
            m.driftMsPerMin = medianOf(residuals) / typicalDelta * 60000.0;

        if (m.nonMonotonicCount > 0)
            addIssue(rep, "error", "pts-nonmonotonic",
                     "流 #" + std::to_string(idx) + " 有 " +
                         std::to_string(m.nonMonotonicCount) + " 处 pts/dts 回退",
                     idx, -1, m.firstPtsSeconds);

        rep.streams.push_back(m);
    }

    // Per-segment A/V phase shift: compare each stream's earliest pts with the
    // reference stream's earliest pts INSIDE THE SAME SEGMENT. A change of that
    // phase between segments means the recorder re-based one stream (a classic
    // HLS "two recordings, pictures not in sync" symptom). Constant biases
    // (AAC priming, edit list) cancel out and produce no issue.
    if (!segs.empty() && refStream >= 0) {
        std::map<int, std::vector<std::pair<int, double>>> shifts;
        for (const auto& s : segs) {
            const auto refIt = s.firstPtsByStream.find(refStream);
            if (refIt == s.firstPtsByStream.end()) continue;
            for (const auto& [si, t] : s.firstPtsByStream) {
                if (si == refStream) continue;
                shifts[si].emplace_back(s.index, (t - refIt->second) * 1000.0);
            }
        }
        for (const auto& [si, v] : shifts) {
            for (size_t k = 1; k < v.size(); ++k) {
                const double d = v[k].second - v[k - 1].second;
                if (std::abs(d) < 100.0) continue;
                addIssue(rep, "warning", "segment-av-shift",
                         "流 #" + std::to_string(si) + " 在分片 #" +
                             std::to_string(v[k].first) +
                             " 处相对参考流的相位跳变 " + msText(d) + " ms",
                         si, v[k].first,
                         v[k].first >= 0 &&
                                 v[k].first < static_cast<int>(segs.size())
                             ? segs[static_cast<size_t>(v[k].first)]
                                   .playlistStartSeconds
                             : -1.0);
            }
        }
    }

    if (!rep.streams.empty()) {
        double first = std::numeric_limits<double>::max();
        double last = -std::numeric_limits<double>::max();
        for (const auto& m : rep.streams) {
            first = std::min(first, m.firstPtsSeconds);
            last = std::max(last, m.lastPtsSeconds);
        }
        rep.coveredSeconds = std::max(0.0, last - first);
    }

    // --- A/V start offset ---------------------------------------------------
    for (const auto& m : rep.streams) {
        if (m.streamIndex == refStream) continue;
        const double off = m.startOffsetMs;
        if (std::abs(off) >= 250.0)
            addIssue(rep, "error", "av-offset",
                     "流 #" + std::to_string(m.streamIndex) + " 相对参考流 #" +
                         std::to_string(refStream) + " 起始偏移 " + msText(off) +
                         " ms，音画明显不同步",
                     m.streamIndex, -1, m.firstPtsSeconds);
        else if (std::abs(off) >= 80.0)
            addIssue(rep, "warning", "av-offset",
                     "流 #" + std::to_string(m.streamIndex) + " 相对参考流 #" +
                         std::to_string(refStream) + " 起始偏移 " + msText(off) +
                         " ms，请核对 AAC priming / edit list",
                     m.streamIndex, -1, m.firstPtsSeconds);
    }
    for (const auto& m : rep.streams) {
        if (std::abs(m.driftMsPerMin) >= 60.0)
            addIssue(rep, "warning", "clock-drift",
                     "流 #" + std::to_string(m.streamIndex) +
                         " 时间戳相对声明时长漂移 " + msText(m.driftMsPerMin) +
                         " ms/min，长时间播放会累积成音画不同步",
                     m.streamIndex, -1, m.firstPtsSeconds);
    }

    // --- segment-level checks ----------------------------------------------
    if (!segs.empty()) {
        if (!rep.playlist.hasPdt())
            addIssue(rep, "info", "timeline-relative",
                     "该清单无 #EXT-X-PROGRAM-DATE-TIME：分片与音画诊断正常可用，"
                     "但无法与另一份录制文件做绝对时间对齐；录制端补上 PDT 后自动升级",
                     -1, -1, -1.0);
        else if (!rep.playlist.allPdt())
            addIssue(rep, "warning", "pdt-partial",
                     "仅 " + std::to_string(rep.playlist.pdtCount) + "/" +
                         std::to_string(segs.size()) +
                         " 个分片带 PDT，绝对时间轴不完整",
                     -1, -1, -1.0);

        for (size_t i = 0; i < segs.size(); ++i) {
            const auto& s = segs[i];
            if (!s.opened)
                addIssue(rep, "error", "segment-missing",
                         "清单第 " + std::to_string(s.index) + " 个分片未被读取：" +
                             s.uri + "（文件缺失或下载失败）",
                         -1, s.index, s.playlistStartSeconds);
            if (s.opened && s.firstPtsSeconds && s.lastPtsSeconds &&
                s.extinfSeconds > 0.2) {
                const double measured = *s.lastPtsSeconds - *s.firstPtsSeconds;
                const double diff = std::abs(measured - s.extinfSeconds);
                if (diff > 0.2 && diff > 0.2 * s.extinfSeconds)
                    addIssue(rep, "warning", "segment-duration-mismatch",
                             "分片 #" + std::to_string(s.index) + " 实测 " +
                                 secText(measured) + " s 与 EXTINF " +
                                 secText(s.extinfSeconds) + " s 不符",
                             -1, s.index, s.playlistStartSeconds);
            }
        }

        // PDT continuity: a gap here is a real recording outage, not a
        // harmless timestamp wrap.
        for (size_t i = 0; i + 1 < segs.size(); ++i) {
            const auto& a = segs[i];
            const auto& b = segs[i + 1];
            if (!a.pdtMs || !b.pdtMs) continue;
            const double expected = a.extinfSeconds * 1000.0;
            const double actual = static_cast<double>(*b.pdtMs - *a.pdtMs);
            if (std::abs(actual - expected) > 250.0)
                addIssue(rep, "warning", "pdt-gap",
                         "分片 #" + std::to_string(b.index) + " 的 PDT 与前一分片相差 " +
                             msText(actual) + " ms，EXTINF 预期 " +
                             msText(expected) + " ms，疑似录制中断或丢片",
                         -1, b.index, b.playlistStartSeconds);
        }

        // A large timestamp jump must be declared with #EXT-X-DISCONTINUITY.
        // Two complementary views are needed:
        //   (1) segment first-pts vs the playlist's elapsed clock,
        //   (2) the ACTUAL per-packet jump attributed to the segment it lands
        //       in. This catches jumps INSIDE a segment too - the field case
        //       that showed a 1.03 s dtsDelta while the tool reported "ok".
        // One issue per segment, so audio+video both seeing the same jump do
        // not produce duplicate alarms.
        std::set<int> reportedSegments;
        auto emitDiscontinuity = [&](int segIdx, double jump,
                                     const std::string& where) {
            if (!reportedSegments.insert(segIdx).second) return;
            const bool declared = segIdx >= 0 &&
                segIdx < static_cast<int>(segs.size()) &&
                segs[static_cast<size_t>(segIdx)].discontinuity;
            const double at = (segIdx >= 0 &&
                               segIdx < static_cast<int>(segs.size()))
                                  ? segs[static_cast<size_t>(segIdx)]
                                        .playlistStartSeconds
                                  : -1.0;
            const std::string segText = segIdx >= 0
                ? "分片 #" + std::to_string(segIdx) : "起始处";
            if (declared)
                addIssue(rep, "info", "declared-discontinuity",
                         segText + " " + where + "时间戳跳变 " + secText(jump) +
                             " s，已标注 #EXT-X-DISCONTINUITY，属预期",
                         -1, segIdx, at);
            else
                addIssue(rep, "error", "unmarked-discontinuity",
                         segText + " " + where + "时间戳跳变 " + secText(jump) +
                             " s 但未标注 #EXT-X-DISCONTINUITY，"
                             "播放器会在此处出现音画/画面不同步",
                         -1, segIdx, at);
        };

        int prev = -1;
        for (size_t i = 0; i < segs.size(); ++i) {
            if (!segs[i].opened || !segs[i].firstPtsSeconds) continue;
            if (prev >= 0) {
                const auto& a = segs[prev];
                const auto& b = segs[i];
                const double expected =
                    b.playlistStartSeconds - a.playlistStartSeconds;
                const double actual = *b.firstPtsSeconds - *a.firstPtsSeconds;
                const double jump = actual - expected;
                if (std::abs(jump) > 0.25)
                    emitDiscontinuity(b.index, jump, "跨分片时钟");
            }
            prev = static_cast<int>(i);
        }

        for (const auto& [idx, rows] : byStream) {
            (void)idx;
            for (size_t k = 1; k < rows.size(); ++k) {
                const auto t0 = rows[k - 1]->ptsTime ? rows[k - 1]->ptsTime
                                                     : rows[k - 1]->dtsTime;
                const auto t1 = rows[k]->ptsTime ? rows[k]->ptsTime
                                                 : rows[k]->dtsTime;
                if (!t0 || !t1) continue;
                const double gap = *t1 - *t0;
                if (gap <= 0.5) continue;
                const int segIdx = rows[k]->segmentIndex >= 0
                                       ? rows[k]->segmentIndex
                                       : rows[k - 1]->segmentIndex;
                emitDiscontinuity(segIdx, gap, "包级");
            }
        }
    }
    return rep;
}

// -------------------------------------------------------------- comparison --

TimelineComparison compareTimelines(const AlignmentReport& a,
                                    const AlignmentReport& b,
                                    std::optional<double> manualAnchorMs) {
    TimelineComparison c;
    c.firstWallClockA = a.firstWallClockMs;
    c.firstWallClockB = b.firstWallClockMs;

    const bool aEmpty = a.streams.empty() && a.coveredSeconds <= 0.0;
    const bool bEmpty = b.streams.empty() && b.coveredSeconds <= 0.0;
    if (aEmpty || bEmpty) {
        c.method = "unavailable";
        c.confidence = 0.0;
        c.note = "其中一份文件没有可分析的时间轴";
        return c;
    }

    const double overlap = std::min(a.coveredSeconds, b.coveredSeconds);
    c.overlapSeconds = overlap;

    if (a.crossFileComparable && b.crossFileComparable &&
        a.firstWallClockMs && b.firstWallClockMs) {
        c.comparable = true;
        c.method = "pdt";
        c.confidence = 1.0;
        c.offsetMs = static_cast<double>(*a.firstWallClockMs -
                                         *b.firstWallClockMs);
        const std::string ta = formatEpochMsUtc(*a.firstWallClockMs);
        const std::string tb = formatEpochMsUtc(*b.firstWallClockMs);
        if (a.lastWallClockMs && b.lastWallClockMs) {
            const int64_t lo = std::max(*a.firstWallClockMs, *b.firstWallClockMs);
            const int64_t hi = std::min(*a.lastWallClockMs, *b.lastWallClockMs);
            if (hi > lo) c.overlapSeconds = static_cast<double>(hi - lo) / 1000.0;
        }
        c.note = "A=" + ta + "，B=" + tb + "，A 相对 B " +
                 msText(c.offsetMs) + " ms（PDT 绝对墙上时钟）";
        return c;
    }

    if (manualAnchorMs) {
        c.comparable = true;
        c.method = "manual-anchor";
        c.confidence = 0.5;
        c.offsetMs = *manualAnchorMs;
        c.note = "缺少 PDT，按人工锚点 " + msText(*manualAnchorMs) +
                 " ms 对齐；该偏移依赖人工判断，置信度中等";
        return c;
    }

    c.method = "relative";
    c.confidence = 0.0;
    c.offsetMs = 0.0;
    c.note = "至少一份录制文件缺少可用的 #EXT-X-PROGRAM-DATE-TIME，"
             "只能按各自起点做相对对齐：画面差异不能直接解释为时间偏差。"
             "请给录制端补 PDT，或用 --anchor-ms 指定一个共同事件作为锚点";
    return c;
}

} // namespace ma
