// ma::infra - ProbeAnalyzer: open -> find_stream_info -> demux (+ optional decode).
//
// Design points that fix the legacy app's defects:
//  * every stream is modeled (video/audio/subtitle/data/attachment), none assumed
//  * packet-level metadata first; frame type comes from a cheap bitstream parser
//    (no pixel decoding); decoding is opt-in, bounded, and only ADDS geometry/
//    format extras - it never replaces a packet's timing, type or samples
//  * staged timeouts + cooperative cancel, no fixed 6 s kill switch
//  * every FFmpeg error becomes a structured Diagnostic with stage + hint
//  * frames are bounded by maxFrames instead of unbounded vectors
#include "ma/infra/ProbeAnalyzer.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "ma/infra/Ffmpeg.hpp"
}

#include "ma/core/UrlNormalizer.hpp"
#include "ma/infra/Interrupt.hpp"
#include "ma/infra/Raii.hpp"

namespace ma::infra {
namespace {

using clock = std::chrono::steady_clock;

double elapsedSince(clock::time_point t) {
    return std::chrono::duration<double>(clock::now() - t).count();
}

double tbToSeconds(const AVRational& tb, int64_t v) {
    if (v == AV_NOPTS_VALUE || tb.den == 0 || v == INT64_MIN) return 0.0;
    return av_q2d(tb) * static_cast<double>(v);
}

StreamKind mapKind(AVMediaType t) {
    switch (t) {
    case AVMEDIA_TYPE_VIDEO: return StreamKind::Video;
    case AVMEDIA_TYPE_AUDIO: return StreamKind::Audio;
    case AVMEDIA_TYPE_SUBTITLE: return StreamKind::Subtitle;
    case AVMEDIA_TYPE_DATA: return StreamKind::Data;
    case AVMEDIA_TYPE_ATTACHMENT: return StreamKind::Attachment;
    default: return StreamKind::Unknown;
    }
}

StreamInfo describeStream(int index, const AVStream* st) {
    StreamInfo s;
    const AVCodecParameters* par = st->codecpar;
    s.index = index;
    s.kind = mapKind(par->codec_type);
    s.codecName = avcodec_get_name(par->codec_id);
    s.level = par->level;

    if (const char* profile = avcodec_profile_name(par->codec_id, par->profile))
        s.profile = profile; // NULL-safe: empty means "unknown", never crash

    if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
        s.width = par->width;
        s.height = par->height;
        if (const char* pix = av_get_pix_fmt_name(static_cast<AVPixelFormat>(par->format)))
            s.pixFmt = pix;
        if (st->avg_frame_rate.num > 0 && st->avg_frame_rate.den > 0)
            s.frameRate = av_q2d(st->avg_frame_rate);
        else if (st->r_frame_rate.num > 0 && st->r_frame_rate.den > 0)
            s.frameRate = av_q2d(st->r_frame_rate);
    } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
        s.channels = par->ch_layout.nb_channels;
        s.sampleRate = par->sample_rate;
        if (const char* sf = av_get_sample_fmt_name(static_cast<AVSampleFormat>(par->format)))
            s.sampleFmt = sf;
    }

    if (st->time_base.den > 0)
        s.timeBase = std::to_string(st->time_base.num) + "/" +
                     std::to_string(st->time_base.den);

    if (st->duration != AV_NOPTS_VALUE && st->time_base.den > 0)
        s.duration = av_q2d(st->time_base) * static_cast<double>(st->duration);
    s.nbFrames = st->nb_frames;
    s.bitrate = par->bit_rate;
    return s;
}

// Bitstream parsers report I/P/B/S without decoding a single pixel. This is
// what makes frame types available in the default (fast) analysis path.
struct ParserState {
    ParserPtr parser;
    CodecContextPtr ctx; // allocated but never opened; parsers read extradata
    bool attempted = false;
};

// Frame-level extras gathered by decoding one packet. Timing, picture type and
// sample count are deliberately NOT taken from the decoder: with B-frame
// reordering, the Nth packet does not produce the Nth displayed frame, so
// copying fr->pict_type into a packet row used to pair a frame type with the
// wrong pts/dts (the "IPIBPB..." shift). Geometry/format is GOP-stable, so it
// is safe to attach to the row that fed the decoder. One packet may yield 0..n
// frames; we keep the first frame's extras and never create extra rows.
struct DecodedExtras {
    int frameCount = 0;
    std::optional<int> width;
    std::optional<int> height;
    std::optional<std::string> pixFmt;
    std::optional<std::string> sampleFmt;
    std::optional<int> channels;
};

class ProbeAnalyzer final : public ma::Analyzer {
public:
    Outcome<AnalysisResult> analyze(const std::string& rawInput,
                                    const AnalysisOptions& opts,
                                    const CancelFlag* cancel,
                                    const ProgressFn& onProgress) override {
        initFfmpeg();
        const auto t0 = clock::now();

        // A controller may reuse this analyzer for several files. Decoder and
        // parser state is strictly per-run and must never leak across inputs.
        decoders_.clear();
        parsers_.clear();
        failedDecoders_.clear();

        Outcome<AnalysisResult> out;
        Diagnostic& diag = out.diag;

        if (rawInput.empty()) {
            diag = {Errc::InvalidArgument, Stage::NormalizeUrl, 0, "empty input", "", "", 0};
            return out;
        }

        const NormalizedUrl norm = normalizeUrl(rawInput);
        diag.url = norm.url;

        InterruptGuard guard(cancel, InterruptGuard::Timeouts{
                                 opts.connectTimeout, opts.stallTimeout,
                                 opts.probeTimeout});

        OpenFormatContext fmt;
        // Allocate the context first so the interrupt callback is already
        // active DURING avformat_open_input. Otherwise a slow HTTPS connect
        // cannot be cancelled or timed out at all - the user would be stuck
        // looking at "Cancelling..." until the TCP/TLS handshake finishes.
        AVFormatContext* raw = avformat_alloc_context();
        if (!raw) {
            diag = {Errc::Internal, Stage::Open, 0, "cannot allocate format context",
                    "", norm.url, elapsedSince(t0)};
            return out;
        }
        guard.attach(raw);

        OptionsDict dict;
        if (opts.probesize > 0)
            dict.set("probesize", std::to_string(opts.probesize));
        if (opts.analyzeduration > 0)
            dict.set("analyzeduration", std::to_string(opts.analyzeduration));
        for (const auto& [k, v] : opts.extraOptions)
            dict.set(k, v);

        // ---- stage 1: open --------------------------------------------------
        int ret = avformat_open_input(&raw, norm.url.c_str(), nullptr, dict.put());
        if (ret < 0) {
            // Per FFmpeg API docs a caller-supplied context is freed on failure,
            // so there is nothing left to release here.
            diag = failureAfterInterrupt(guard, Stage::Open, ret, norm);
            diag.elapsedSeconds = elapsedSince(t0);
            return out;
        }
        fmt.reset(raw); // take ownership once open succeeded
        guard.onOpened();

        if (!dict.empty())
            diag.hint = "unused options: " + summarizeDict(dict.releaseLeftovers());

        // ---- stage 2: stream info -------------------------------------------
        // A live HLS / high-latency RTSP probe can legitimately run for tens
        // of seconds without emitting a single packet. Bracket it so the
        // per-packet stall watchdog cannot fire while probing, while a
        // dedicated probe ceiling still guarantees we never hang forever.
        guard.beginProbe();
        ret = avformat_find_stream_info(fmt.get(), nullptr);
        guard.endProbe();
        if (ret < 0 && fmt.get()->nb_streams == 0) {
            diag = failureAfterInterrupt(guard, Stage::FindStreamInfo, ret, norm);
            diag.elapsedSeconds = elapsedSince(t0);
            return out;
        }
        // ret < 0 with streams present: keep going, metadata is good enough.

        // ---- stage 3: describe container + streams --------------------------
        AnalysisResult& r = out.value;
        AVFormatContext* f = fmt.get();
        r.formatName = f->iformat ? f->iformat->name : "";
        r.formatLongName = f->iformat ? f->iformat->long_name : "";
        if (f->duration != AV_NOPTS_VALUE)
            r.duration = static_cast<double>(f->duration) / AV_TIME_BASE;
        if (f->start_time != AV_NOPTS_VALUE)
            r.startSeconds = static_cast<double>(f->start_time) / AV_TIME_BASE;
        r.bitrate = f->bit_rate;

        for (unsigned i = 0; i < f->nb_streams; ++i)
            r.streams.push_back(describeStream(static_cast<int>(i), f->streams[i]));

        // ---- stage 4: demux loop --------------------------------------------
        PacketPtr pkt{av_packet_alloc()};
        std::map<int, std::optional<double>> lastDtsTime; // per-stream, for delta
        int64_t packets = 0;
        int64_t seq = 0;            // row number actually emitted
        int64_t decodedFrames = 0;  // total decoded frames, for the hint
        int64_t decodedRows = 0;    // rows that actually received extras
        // Budget is counted PER STREAM. With a single global counter an
        // audio-heavy (or badly interleaved) container can consume the entire
        // quota before the first video packet arrives, leaving the video tab
        // with no frame-level information at all - exactly the "all decode
        // info disappeared" symptom. Per-stream counting guarantees the first
        // N decoded frames of every video/audio stream get their extras.
        std::map<int, int64_t> decodedPerStream;
        bool decodeLimited = false;

        while (true) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                finishCancelled(diag, norm.url, t0, r, packets);
                return out;
            }
            if (static_cast<int64_t>(r.frames.size()) >= opts.maxFrames) {
                r.truncated = true;
                break;
            }

            ret = av_read_frame(f, pkt.get());
            if (ret < 0) {
                if (ret == AVERROR_EOF || avio_feof(f->pb)) {
                    // A live source often reports a clean EOF when the server
                    // disconnects. If the user had already pressed Stop, that
                    // must still be reported as a partial, user-stopped run
                    // rather than "Done" - otherwise the UI lies about the
                    // result being complete.
                    if (cancel && cancel->load(std::memory_order_relaxed)) {
                        finishCancelled(diag, norm.url, t0, r, packets);
                        return out;
                    }
                    break; // clean end
                }
                if (guard.triggeredReason() == Errc::Cancelled) {
                    finishCancelled(diag, norm.url, t0, r, packets);
                } else if (guard.triggeredReason() == Errc::Timeout) {
                    diag = {Errc::Timeout, Stage::Read, 0,
                            "no data within stall timeout", "",
                            norm.url, elapsedSince(t0)};
                } else {
                    diag = makeFfmpegDiagnostic(Stage::Read, ret, norm.url,
                                                "av_read_frame");
                }
                diag.elapsedSeconds = elapsedSince(t0);
                return out;
            }

            guard.onActivity();
            ++packets;
            if (onProgress && (packets % 64) == 0)
                onProgress({packets, elapsedSince(t0)});

            AVStream* st = f->streams[pkt->stream_index];
            const AVRational& tb = st->time_base;
            const AVCodecParameters* par = st->codecpar;
            const std::optional<double> dtsTime =
                pkt->dts == AV_NOPTS_VALUE
                    ? std::nullopt
                    : std::optional<double>{tbToSeconds(tb, pkt->dts)};

            FrameRecord rec = packetToRecord(pkt.get(), st, seq + 1);

            // Cheap I/P/B detection: no pixel decoding, works for H.264/H.265,
            // MPEG-1/2/4, VP9, AV1, ... Falls back to the keyframe marker.
            if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
                if (auto type = probeVideoType(f, pkt.get())) rec.pictType = *type;
            }

            if (dtsTime && lastDtsTime[pkt->stream_index])
                rec.dtsDelta = *dtsTime - *lastDtsTime[pkt->stream_index];
            lastDtsTime[pkt->stream_index] = dtsTime;

            const bool decodable =
                par->codec_type == AVMEDIA_TYPE_VIDEO ||
                par->codec_type == AVMEDIA_TYPE_AUDIO;
            const int streamIdx = pkt->stream_index;
            const bool decodeThis = opts.decodeFrames && decodable &&
                                    decodedPerStream[streamIdx] <
                                        opts.maxDecodeFrames;
            if (opts.decodeFrames && decodable && !decodeThis) decodeLimited = true;

            // Exceeding the budget must NOT discard the decode information
            // already gathered: those rows keep their extras, only later rows
            // stay packet-level. This is the "show the first N decoded frames"
            // contract.
            if (decodeThis) {
                const DecodedExtras ex = decodeExtras(pkt.get(), f);
                if (ex.frameCount > 0) {
                    rec.decoded = true;
                    rec.width = ex.width;
                    rec.height = ex.height;
                    rec.pixFmt = ex.pixFmt;
                    rec.sampleFmt = ex.sampleFmt;
                    rec.channels = ex.channels;
                    decodedPerStream[streamIdx] += ex.frameCount;
                    decodedFrames += ex.frameCount;
                    ++decodedRows;
                }
            }
            // Exactly one row per packet, decoded or not. Timing, seq, type and
            // samples always stay the packet's own values - no frame/packet
            // mismatch, and the table shape never depends on decodeFrames.
            r.frames.push_back(std::move(rec));
            ++seq;
        }

        r.totalPacketsRead = packets;

        // Drain decoders at EOF. B-frame reordering keeps a few frames buffered
        // inside the decoder; without this flush the tail is silently lost and
        // a diagnostic run cannot tell a clean decode from a truncated one.
        // Cancellation/error paths return early on purpose: flushing a broken
        // stream must not delay the user's Stop.
        int64_t flushedFrames = 0;
        if (opts.decodeFrames) {
            for (auto& kv : decoders_) {
                CodecContextPtr& cc = kv.second;
                if (!cc) continue;
                if (avcodec_send_packet(cc.get(), nullptr) < 0) continue;
                FramePtr frame{av_frame_alloc()};
                if (!frame) break;
                while (avcodec_receive_frame(cc.get(), frame.get()) == 0) {
                    ++flushedFrames;
                    av_frame_unref(frame.get());
                }
            }
        }

        diag.code = Errc::Ok;
        diag.stage = Stage::None;
        diag.url = norm.url;
        diag.elapsedSeconds = elapsedSince(t0);
        if (norm.changed && !norm.notes.empty()) diag.hint = norm.notes;
        if (opts.decodeFrames) {
            auto append = [&](const std::string& s) {
                if (!diag.hint.empty()) diag.hint += "; ";
                diag.hint += s;
            };
            std::string stats = "decode: " + std::to_string(decodedFrames) +
                                " frames";
            if (flushedFrames > 0)
                stats += " + " + std::to_string(flushedFrames) +
                         " tail frames flushed";
            if (!failedDecoders_.empty()) {
                stats += ", no decoder on stream";
                for (int i : failedDecoders_) stats += " " + std::to_string(i);
            }
            append(stats);
            if (decodeLimited)
                append("frame-level dims/format/ch ARE shown on the first " +
                       std::to_string(decodedRows) + " rows (maxDecodeFrames=" +
                       std::to_string(opts.maxDecodeFrames) +
                       " per stream); later rows stay packet-level only");
        }
        return out;
    }

private:
    static void finishCancelled(Diagnostic& diag, const std::string& url,
                                clock::time_point t0, AnalysisResult& r,
                                int64_t packets) {
        // A stop is a valid, partial result - not an error. The hint is shown
        // in the UI's yellow notes line so the user can tell a deliberately
        // truncated run from a complete one.
        diag = {Errc::Cancelled, Stage::Read, 0, "cancelled by user",
                "analysis stopped by user after " + std::to_string(packets) +
                    " packets; showing partial results (no later frames parsed)",
                url, elapsedSince(t0)};
        r.totalPacketsRead = packets;
    }

    // Packet-level row. Fills sample count for audio and a keyframe-based
    // "I" for video even when no decoder/parser is available.
    static FrameRecord packetToRecord(const AVPacket* pkt, const AVStream* st,
                                      int64_t seq) {
        FrameRecord rec;
        rec.seq = seq;
        rec.streamIndex = pkt->stream_index;
        const AVRational& tb = st->time_base;
        const AVCodecParameters* par = st->codecpar;

        rec.dts = pkt->dts;
        rec.pts = pkt->pts;
        if (pkt->dts != AV_NOPTS_VALUE) rec.dtsTime = tbToSeconds(tb, pkt->dts);
        if (pkt->pts != AV_NOPTS_VALUE) rec.ptsTime = tbToSeconds(tb, pkt->pts);
        rec.duration = pkt->duration;
        if (pkt->duration > 0 && tb.den > 0)
            rec.durationTime = av_q2d(tb) * static_cast<double>(pkt->duration);
        rec.size = pkt->size;
        rec.keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;

        // Audio packets carry duration in stream time_base; converting to a
        // sample time base yields exactly nb_samples (AAC 1024, MP3 1152, ...).
        if (par->codec_type == AVMEDIA_TYPE_AUDIO && pkt->duration > 0 &&
            par->sample_rate > 0 && tb.num > 0 && tb.den > 0) {
            const AVRational sampleTb{1, par->sample_rate};
            const int64_t n = av_rescale_q(pkt->duration, tb, sampleTb);
            if (n > 0) rec.samples = n;
        }
        if (par->codec_type == AVMEDIA_TYPE_VIDEO && rec.keyframe)
            rec.pictType = std::string("I");
        return rec;
    }

    std::optional<std::string> probeVideoType(AVFormatContext* f, AVPacket* pkt) {
        const int idx = pkt->stream_index;
        ParserState& ps = parsers_[idx];
        if (!ps.attempted) {
            ps.attempted = true;
            const AVCodecParameters* par = f->streams[idx]->codecpar;
            if (AVCodecParserContext* p = av_parser_init(par->codec_id)) {
                ps.parser.reset(p);
                if (AVCodecContext* c = avcodec_alloc_context3(nullptr)) {
                    ps.ctx.reset(c);
                    avcodec_parameters_to_context(c, par);
                }
            }
            // A parser needs a codec context for extradata; without one, skip.
            if (!ps.ctx) ps.parser.reset();
        }
        if (!ps.parser) return std::nullopt;

        uint8_t* out = nullptr;
        int outSize = 0;
        const int used = av_parser_parse2(ps.parser.get(), ps.ctx.get(),
                                          &out, &outSize,
                                          pkt->data, pkt->size,
                                          pkt->pts, pkt->dts, pkt->pos);
        if (used < 0) return std::nullopt;
        const int pt = ps.parser->pict_type;
        if (pt == AV_PICTURE_TYPE_NONE) return std::nullopt;
        const char c = av_get_picture_type_char(static_cast<AVPictureType>(pt));
        if (!c || c == '?') return std::nullopt;
        return std::string(1, c);
    }

    Diagnostic failureAfterInterrupt(const InterruptGuard& guard, Stage stage,
                                     int ret, const NormalizedUrl& norm) {
        const Errc reason = guard.triggeredReason();
        if (reason == Errc::Timeout) {
            Diagnostic d;
            d.code = Errc::Timeout;
            d.stage = stage;
            d.url = norm.url;
            const double limit = stage == Stage::Open
                                     ? guard.timeouts().connectSeconds
                                     : (stage == Stage::FindStreamInfo
                                            ? guard.timeouts().probeSeconds
                                            : guard.timeouts().stallSeconds);
            d.message = "timed out after " + std::to_string(limit) + " s" +
                        (stage == Stage::FindStreamInfo ? " while probing streams" : "");
            d.hint = "adjust connect/stall/probe timeout in options";
            return d;
        }
        if (reason == Errc::Cancelled) {
            Diagnostic d;
            d.code = Errc::Cancelled;
            d.stage = stage;
            d.url = norm.url;
            d.message = "cancelled by user";
            d.hint = "stopped during " + std::string(ma::toString(stage)) +
                     " (slow open / probe); no frames were parsed";
            return d;
        }
        Diagnostic d = makeFfmpegDiagnostic(stage, ret, norm.url,
                                            stage == Stage::Open ? "avformat_open_input"
                                                                 : "avformat_find_stream_info");
        if (!norm.notes.empty()) d.hint = (d.hint.empty() ? "" : d.hint + "; ") + norm.notes;
        return d;
    }

    static std::string summarizeDict(AVDictionary* d) {
        std::string out;
        const AVDictionaryEntry* e = nullptr;
        while ((e = av_dict_iterate(d, e))) {
            if (!out.empty()) out += ", ";
            out += e->key;
            out += "=";
            out += e->value;
        }
        av_dict_free(&d);
        return out;
    }

    // Decode one packet and return frame-level EXTRAS only. The row count,
    // seq, dts/pts, pictType and samples are owned exclusively by the packet
    // path; mixing decoder output (display order) into packet rows (decode
    // order) is what caused the old I/P/B vs pts mismatch.
    DecodedExtras decodeExtras(AVPacket* pkt, AVFormatContext* f) {
        DecodedExtras ex;
        const int idx = pkt->stream_index;
        if (failedDecoders_.count(idx)) return ex; // don't retry every packet

        CodecContextPtr& cc = decoders_[idx];
        const AVCodecParameters* par = f->streams[idx]->codecpar;
        const bool isVideo = par->codec_type == AVMEDIA_TYPE_VIDEO;
        if (!cc) {
            const AVCodec* dec = avcodec_find_decoder(par->codec_id);
            if (!dec) { failedDecoders_.insert(idx); return ex; }
            AVCodecContext* c = avcodec_alloc_context3(dec);
            if (!c) { failedDecoders_.insert(idx); return ex; }
            cc.reset(c);
            if (avcodec_parameters_to_context(c, par) < 0 ||
                avcodec_open2(c, dec, nullptr) < 0) {
                cc.reset();
                failedDecoders_.insert(idx);
                return ex;
            }
        }

        FramePtr frame{av_frame_alloc()};
        if (!frame) return ex;

        auto drain = [&]() {
            while (avcodec_receive_frame(cc.get(), frame.get()) == 0) {
                ++ex.frameCount;
                // A packet can yield several frames; geometry/format is stable
                // within a GOP, so the first frame is representative.
                if (ex.frameCount == 1) {
                    if (isVideo) {
                        if (frame->width > 0) {
                            ex.width = frame->width;
                            ex.height = frame->height;
                        }
                        if (const char* pix = av_get_pix_fmt_name(
                                static_cast<AVPixelFormat>(frame->format)))
                            ex.pixFmt = pix;
                    } else {
                        if (const char* sf = av_get_sample_fmt_name(
                                static_cast<AVSampleFormat>(frame->format)))
                            ex.sampleFmt = sf;
                        ex.channels = frame->ch_layout.nb_channels;
                    }
                }
                av_frame_unref(frame.get());
            }
        };

        // EAGAIN means "input full": drain, then re-send the SAME packet.
        for (int attempt = 0; attempt < 8; ++attempt) {
            const int sent = avcodec_send_packet(cc.get(), pkt);
            drain();
            if (sent != AVERROR(EAGAIN)) break;
        }
        return ex;
    }

    std::map<int, CodecContextPtr> decoders_;
    std::map<int, ParserState> parsers_;
    std::set<int> failedDecoders_;
};

} // namespace

std::unique_ptr<ma::Analyzer> createProbeAnalyzer() {
    return std::make_unique<ProbeAnalyzer>();
}

} // namespace ma::infra
