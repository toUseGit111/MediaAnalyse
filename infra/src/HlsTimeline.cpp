// ma::infra - HLS playlist loader + io_open-based segment tracker.
#include "ma/infra/HlsTimeline.hpp"

#include <algorithm>
#include <cctype>

extern "C" {
#include "ma/infra/Ffmpeg.hpp"
}

namespace ma::infra {
namespace {

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string stripQuery(const std::string& s) {
    std::string out = s;
    if (auto q = out.find('?'); q != std::string::npos) out.resize(q);
    if (auto h = out.find('#'); h != std::string::npos) out.resize(h);
    return out;
}

} // namespace

std::string urlBasename(const std::string& url) {
    std::string s = stripQuery(url);
    if (const auto p = s.find_last_of("/\\"); p != std::string::npos)
        s = s.substr(p + 1);
    return lower(s);
}

bool looksLikeHlsPlaylist(const std::string& url) {
    const std::string s = lower(stripQuery(url));
    const auto slash = s.find_last_of("/\\");
    const auto dot = s.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return false;
    const std::string ext = s.substr(dot + 1);
    return ext == "m3u8" || ext == "m3u";
}

ma::HlsPlaylist loadHlsPlaylist(const std::string& url, std::string* error) {
    ma::HlsPlaylist pl;
    pl.source = url;
    AVIOContext* pb = nullptr;
    AVDictionary* opts = nullptr;
    const int ret = avio_open2(&pb, url.c_str(), AVIO_FLAG_READ, nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0 || !pb) {
        if (error) {
            char b[AV_ERROR_MAX_STRING_SIZE] = {};
            av_strerror(ret, b, sizeof(b));
            *error = std::string("cannot read playlist: ") + b;
        }
        return pl;
    }

    // A playlist is text, not media. 4 MiB is generous (a 24 h VOD at 2 s
    // segments is ~1.5 MiB) and bounds a malicious/looping HTTP response.
    constexpr size_t kMaxPlaylistBytes = 4u * 1024u * 1024u;
    std::string text;
    unsigned char buf[4096];
    while (text.size() < kMaxPlaylistBytes) {
        const int n = avio_read(pb, buf, static_cast<int>(sizeof(buf)));
        if (n <= 0) break;
        text.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
    }
    avio_close(pb);
    if (text.size() >= kMaxPlaylistBytes && error)
        *error = "playlist truncated at 4 MiB";

    pl = ma::parseHlsPlaylist(text, url);
    if (error && pl.empty() && error->empty())
        *error = "playlist contains no media segments";
    return pl;
}

// ---------------------------------------------------------- segment tracker --

void SegmentTracker::attach(AVFormatContext* fmt) {
    if (!fmt) return;
    fmt_ = fmt;
    origIoOpen_ = fmt->io_open;
    fmt->opaque = this;
    fmt->io_open = &SegmentTracker::ioOpenThunk;
}

int SegmentTracker::ioOpenThunk(AVFormatContext* s, AVIOContext** pb,
                                const char* url, int flags,
                                AVDictionary** options) {
    auto* self = s && s->opaque ? static_cast<SegmentTracker*>(s->opaque) : nullptr;
    int ret;
    if (self && self->origIoOpen_)
        ret = self->origIoOpen_(s, pb, url, flags, options);
    else
        ret = avio_open2(pb, url, flags, s ? &s->interrupt_callback : nullptr,
                         options);
    if (ret >= 0 && self && url) self->onOpened(url);
    return ret;
}

int SegmentTracker::matchSegment(const std::string& url) const {
    if (!playlist_) return -1;
    const std::string base = urlBasename(url);
    if (base.empty()) return -1;
    for (const auto& s : playlist_->segments)
        if (urlBasename(s.uri) == base) return s.index;
    return -1;
}

void SegmentTracker::onOpened(const char* url) {
    const std::string u = url ? url : "";
    if (!openedUrls_.empty() && openedUrls_.back() == u) return; // nested re-open
    openedUrls_.push_back(u);

    const int idx = matchSegment(u);
    if (idx >= 0 && playlist_) {
        (*playlist_).segments[static_cast<size_t>(idx)].opened = true;
        currentSegment_ = idx;
    } else if (!playlist_) {
        // Playlist not loaded yet: keep the URL; setPlaylist() replays it.
        currentSegment_ = -1;
    }
}

void SegmentTracker::setPlaylist(ma::HlsPlaylist* pl) {
    playlist_ = pl;
    if (!pl) return;
    // Replay URLs opened during avformat_open_input / find_stream_info. The
    // last match is the segment currently being read.
    for (const auto& u : openedUrls_) {
        const int idx = matchSegment(u);
        if (idx < 0) continue;
        pl->segments[static_cast<size_t>(idx)].opened = true;
        currentSegment_ = idx;
    }
}

void SegmentTracker::onPacket(int streamIndex,
                              const std::optional<double>& ptsTime,
                              const std::optional<double>& dtsTime) {
    if (!playlist_ || currentSegment_ < 0) return;
    if (currentSegment_ >= static_cast<int>(playlist_->segments.size())) return;
    auto& seg = playlist_->segments[static_cast<size_t>(currentSegment_)];
    const auto t = ptsTime ? ptsTime : dtsTime;
    if (!t) return;
    ++seg.packetCount;
    seg.opened = true;
    if (!seg.firstPtsSeconds || *t < *seg.firstPtsSeconds)
        seg.firstPtsSeconds = *t;
    if (!seg.lastPtsSeconds || *t > *seg.lastPtsSeconds)
        seg.lastPtsSeconds = *t;
    // Use the smallest pts of that stream inside the segment: with B-frame
    // reordering the first demuxed packet is not the earliest display time.
    auto it = seg.firstPtsByStream.find(streamIndex);
    if (it == seg.firstPtsByStream.end() || *t < it->second)
        seg.firstPtsByStream[streamIndex] = *t;
}

} // namespace ma::infra
