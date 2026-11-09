#pragma once
// ma::infra - FFmpeg-side helpers for the HLS timeline / alignment feature.
//
// The playlist text is read through FFmpeg's AVIO so local files and HTTP(S)
// URLs share one code path (and inherit the project's existing timeouts).
// SegmentTracker hooks AVFormatContext::io_open: that is the ONLY reliable way
// to know which playlist segment produced a packet. Guessing from pts would
// break exactly on the discontinuity / missing-segment cases we diagnose.
#include <optional>
#include <string>
#include <vector>

#include "ma/core/Alignment.hpp"

struct AVFormatContext;
struct AVIOContext;
struct AVDictionary;

namespace ma::infra {

// Cheap URL heuristic; the demuxer's format name is the authoritative check
// after avformat_open_input() (HLS URLs need not end in .m3u8).
bool looksLikeHlsPlaylist(const std::string& url);

// Read a media playlist via AVIO. On failure `error` is filled and an empty
// (but valid) playlist is returned - a missing playlist must never turn a
// successful media analysis into a failure.
ma::HlsPlaylist loadHlsPlaylist(const std::string& url, std::string* error);

class SegmentTracker {
public:
    using IoOpenFn = int (*)(AVFormatContext*, AVIOContext**, const char*, int,
                             AVDictionary**);

    // Install before avformat_open_input(): find_stream_info() may already
    // open the first segment. `fmt->opaque` is borrowed; FFmpeg propagates it
    // to nested demuxer contexts (documented for io_open).
    void attach(AVFormatContext* fmt);

    // Called once the playlist has been loaded (usually right after open).
    // Replays URLs already opened during probing so no early segment is lost.
    void setPlaylist(ma::HlsPlaylist* pl);

    int currentSegment() const { return currentSegment_; }
    const std::vector<std::string>& openedUrls() const { return openedUrls_; }

    // Accumulate demux-measured facts used by computeAlignment().
    void onPacket(int streamIndex, const std::optional<double>& ptsTime,
                  const std::optional<double>& dtsTime);

private:
    static int ioOpenThunk(AVFormatContext* s, AVIOContext** pb,
                           const char* url, int flags, AVDictionary** options);
    void onOpened(const char* url);
    int matchSegment(const std::string& url) const;

    AVFormatContext* fmt_ = nullptr;
    IoOpenFn origIoOpen_ = nullptr;
    ma::HlsPlaylist* playlist_ = nullptr;
    std::vector<std::string> openedUrls_;
    int currentSegment_ = -1;
};

// Basename (query stripped, lower-cased) used to match FFmpeg's absolute
// segment URLs against the relative URIs inside the playlist.
std::string urlBasename(const std::string& url);

} // namespace ma::infra
