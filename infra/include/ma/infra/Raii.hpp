#pragma once
// RAII guards for FFmpeg C resources. C++20, no exceptions thrown from here.
#include <cstdio>
#include <memory>

#include "ma/infra/Ffmpeg.hpp"

namespace ma::infra {

struct FfmpegDeleters {
    void operator()(AVFormatContext* p) const noexcept {
        if (p) avformat_close_input(&p);
    }
    void operator()(AVCodecContext* p) const noexcept {
        if (p) avcodec_free_context(&p);
    }
    void operator()(AVPacket* p) const noexcept {
        if (p) av_packet_free(&p);
    }
    void operator()(AVFrame* p) const noexcept {
        if (p) av_frame_free(&p);
    }
    void operator()(AVDictionary* p) const noexcept {
        if (p) av_dict_free(&p);
    }
    void operator()(AVIOContext* p) const noexcept {
        // Note: only for contexts we own the buffer of; not used yet.
        (void)p;
    }
};

using FormatContextPtr  = std::unique_ptr<AVFormatContext, FfmpegDeleters>;
using CodecContextPtr   = std::unique_ptr<AVCodecContext, FfmpegDeleters>;
using PacketPtr         = std::unique_ptr<AVPacket, FfmpegDeleters>;
using FramePtr          = std::unique_ptr<AVFrame, FfmpegDeleters>;
using DictionaryPtr     = std::unique_ptr<AVDictionary, FfmpegDeleters>;

// AVFormatContext needs special open semantics; wrap manually.
class OpenFormatContext {
public:
    OpenFormatContext() = default;
    ~OpenFormatContext() { reset(); }

    OpenFormatContext(const OpenFormatContext&) = delete;
    OpenFormatContext& operator=(const OpenFormatContext&) = delete;

    AVFormatContext** put() {
        reset();
        return &ctx_;
    }

    AVFormatContext* get() const noexcept { return ctx_; }

    void reset() noexcept {
        if (ctx_) {
            AVFormatContext* tmp = ctx_;
            ctx_ = nullptr;
            avformat_close_input(&tmp);
        }
    }

    void reset(AVFormatContext* p) noexcept {
        reset();
        ctx_ = p;
    }

    AVFormatContext* release() noexcept {
        AVFormatContext* tmp = ctx_;
        ctx_ = nullptr;
        return tmp;
    }

private:
    AVFormatContext* ctx_ = nullptr;
};

// RAII dictionary that reports unconsumed options.
class OptionsDict {
public:
    OptionsDict() = default;
    ~OptionsDict() { clear(); }

    OptionsDict(const OptionsDict&) = delete;
    OptionsDict& operator=(const OptionsDict&) = delete;

    void set(const std::string& key, const std::string& value, int flags = 0) {
        av_dict_set(&dict_, key.c_str(), value.c_str(), flags);
    }

    AVDictionary** put() { return &dict_; }

    // Move leftover (unknown) options out; caller decides what to do with them.
    AVDictionary* releaseLeftovers() {
        AVDictionary* d = dict_;
        dict_ = nullptr;
        return d;
    }

    void clear() noexcept {
        if (dict_) av_dict_free(&dict_);
        dict_ = nullptr;
    }

    bool empty() const noexcept { return av_dict_count(dict_) == 0; }

private:
    AVDictionary* dict_ = nullptr;
};

} // namespace ma::infra
