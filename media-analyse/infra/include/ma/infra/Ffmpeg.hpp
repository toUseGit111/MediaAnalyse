#pragma once
// Single FFmpeg include point. FFmpeg headers are C; keep global namespace clean.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavutil/rational.h>
#include <libavutil/samplefmt.h>
}

namespace ma::infra {

// Tag with the FFmpeg version actually linked, for diagnostics output.
const char* ffmpegVersionString();

} // namespace ma::infra
