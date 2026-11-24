#include "ma/infra/AudioDecoder.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>

extern "C" {
#include <libswresample/swresample.h>
}

#include "ma/infra/Ffmpeg.hpp"
#include "ma/infra/Interrupt.hpp"
#include "ma/infra/Logger.hpp"
#include "ma/infra/Raii.hpp"

namespace ma::infra {
namespace {

struct SwrDeleter {
    void operator()(SwrContext* p) const noexcept {
        if (p) swr_free(&p);
    }
};
using SwrPtr = std::unique_ptr<SwrContext, SwrDeleter>;

Diagnostic fail(Stage stage, Errc code, const std::string& msg,
                const std::string& path) {
    Diagnostic d;
    d.stage = stage;
    d.code = code;
    d.message = msg;
    d.url = path;
    return d;
}

}  // namespace

Outcome<std::vector<float>> decodeAudioMono(
    const std::string& path, const CancelFlag* cancel,
    const AnalysisOptions& options, double maxSeconds, int targetRate) {
    if (targetRate <= 0) targetRate = 16000;
    Outcome<std::vector<float>> out;
    if (path.empty()) {
        out.diag = fail(Stage::NormalizeUrl, Errc::InvalidArgument,
                        "音频路径为空", path);
        return out;
    }
    if (cancel && cancel->load()) {
        out.diag = fail(Stage::Open, Errc::Cancelled, "已取消", path);
        return out;
    }

    initFfmpeg();
    InterruptGuard::Timeouts timeouts;
    timeouts.connectSeconds = options.connectTimeout;
    timeouts.stallSeconds = options.stallTimeout;
    timeouts.probeSeconds = options.probeTimeout;
    InterruptGuard guard(cancel, timeouts);

    OpenFormatContext fmt;
    AVDictionary* opts = nullptr;
    // Reuse the caller's FFmpeg options (e.g. rtsp_transport) for the audio
    // pass so a network stream is reached the same way as the video pass.
    for (const auto& kv : options.extraOptions)
        av_dict_set(&opts, kv.first.c_str(), kv.second.c_str(), 0);
    int ret = avformat_open_input(fmt.put(), path.c_str(), nullptr,
                                  opts ? &opts : nullptr);
    if (opts) av_dict_free(&opts);
    guard.attach(fmt.get());
    if (ret < 0) {
        out.diag = makeFfmpegDiagnostic(Stage::Open, ret, path, "打开音频失败");
        return out;
    }
    guard.onOpened();

    ret = avformat_find_stream_info(fmt.get(), nullptr);
    if (ret < 0) {
        out.diag = makeFfmpegDiagnostic(Stage::FindStreamInfo, ret, path,
                                        "音频流探测失败");
        return out;
    }

    const int streamIdx = av_find_best_stream(
        fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (streamIdx < 0) {
        out.diag = fail(Stage::FindStreamInfo, Errc::InvalidArgument,
                        "文件没有音频流，无法做音频互相关对齐", path);
        return out;
    }
    AVStream* stream = fmt.get()->streams[streamIdx];
    const AVCodec* decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!decoder) {
        out.diag = fail(Stage::FindStreamInfo, Errc::UnsupportedScheme,
                        std::string("找不到音频解码器: ") +
                            avcodec_get_name(stream->codecpar->codec_id),
                        path);
        return out;
    }

    CodecContextPtr codec(avcodec_alloc_context3(decoder));
    if (!codec) {
        out.diag = fail(Stage::FindStreamInfo, Errc::Internal,
                        "无法分配音频解码器上下文", path);
        return out;
    }
    ret = avcodec_parameters_to_context(codec.get(), stream->codecpar);
    if (ret < 0) {
        out.diag = makeFfmpegDiagnostic(Stage::FindStreamInfo, ret, path,
                                        "复制音频解码参数失败");
        return out;
    }
    ret = avcodec_open2(codec.get(), decoder, nullptr);
    if (ret < 0) {
        out.diag = makeFfmpegDiagnostic(Stage::FindStreamInfo, ret, path,
                                        "打开音频解码器失败");
        return out;
    }

    const int64_t srcRate = codec->sample_rate > 0 ? codec->sample_rate : 48000;
    const AVChannelLayout mono =
        AV_CHANNEL_LAYOUT_MONO;
    SwrContext* rawSwr = nullptr;
    AVChannelLayout srcLayout = codec->ch_layout;
    if (swr_alloc_set_opts2(&rawSwr, &mono, AV_SAMPLE_FMT_FLT, targetRate,
                            &srcLayout,
                            static_cast<AVSampleFormat>(codec->sample_fmt),
                            static_cast<int>(srcRate), 0, nullptr) < 0 ||
        !rawSwr || swr_init(rawSwr) < 0) {
        if (rawSwr) swr_free(&rawSwr);
        out.diag = fail(Stage::Decode, Errc::UnsupportedScheme,
                        "无法初始化音频重采样器（单声道 float）", path);
        return out;
    }
    SwrPtr swr(rawSwr);

    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!packet || !frame) {
        out.diag = fail(Stage::Decode, Errc::Internal,
                        "无法分配音频包/帧", path);
        return out;
    }

    const int64_t maxSamples = static_cast<int64_t>(maxSeconds * targetRate);
    std::vector<float> pcm;
    pcm.reserve(static_cast<size_t>(
        std::min<int64_t>(maxSamples, static_cast<int64_t>(targetRate) * 60)));

    while (pcm.size() < static_cast<size_t>(maxSamples)) {
        if (cancel && cancel->load()) {
            out.diag = fail(Stage::Decode, Errc::Cancelled, "音频解码已取消", path);
            return out;
        }
        ret = av_read_frame(fmt.get(), packet.get());
        if (ret == AVERROR_EOF) break;
        if (ret < 0) {
            out.diag = guard.triggeredReason() != Errc::Ok
                           ? fail(Stage::Decode, guard.triggeredReason(),
                                  "音频读取超时或取消", path)
                           : makeFfmpegDiagnostic(Stage::Decode, ret, path,
                                                  "读取音频包失败");
            return out;
        }
        guard.onActivity();
        if (packet->stream_index != streamIdx) {
            av_packet_unref(packet.get());
            continue;
        }
        ret = avcodec_send_packet(codec.get(), packet.get());
        av_packet_unref(packet.get());
        if (ret < 0) continue;

        while (ret >= 0 && pcm.size() < static_cast<size_t>(maxSamples)) {
            ret = avcodec_receive_frame(codec.get(), frame.get());
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
            if (ret < 0) continue;

            // Convert to 16 kHz mono float.
            const int outSamples = swr_get_out_samples(swr.get(), frame->nb_samples);
            if (outSamples <= 0) continue;
            std::vector<float> buf(static_cast<size_t>(outSamples));
            uint8_t* outPtr[1] = { reinterpret_cast<uint8_t*>(buf.data()) };
            const int converted = swr_convert(
                swr.get(), outPtr, outSamples,
                reinterpret_cast<const uint8_t* const*>(frame->extended_data),
                frame->nb_samples);
            if (converted > 0) {
                const size_t room = static_cast<size_t>(maxSamples) - pcm.size();
                const size_t n = std::min<size_t>(
                    static_cast<size_t>(converted), room);
                pcm.insert(pcm.end(), buf.begin(), buf.begin() + n);
            }
            av_frame_unref(frame.get());
        }
    }

    if (pcm.size() < 1600) {  // < 0.1 s is useless for correlation
        out.diag = fail(Stage::Decode, Errc::InvalidArgument,
                        "音频样本太少（小于 0.1 秒），无法互相关", path);
        return out;
    }
    out.value = std::move(pcm);
    out.diag.code = Errc::Ok;
    out.diag.stage = Stage::None;
    MA_LOGI("audio", "decoded " << out.value.size() << " samples ("
                                << (out.value.size() /
                                    static_cast<double>(targetRate))
                                << " s @ " << targetRate << " Hz) from "
                                << redact(path));
    return out;
}

}  // namespace ma::infra
