#pragma once
// Synchronous, per-packet I/P/B detection from the packet's OWN bitstream.
//
// Why this exists: AVCodecParserContext::pict_type (and AVFrame::pict_type)
// describe the frame that was just *completed*, which lags the packet being
// demuxed. Worse, the parser keeps the previous pict_type until a new frame
// is recognised. Copying that value onto the current row produced the
// "3 I frames at startup, 2 at every GOP start" bug. Live snapshots are
// published incrementally and can never be back-filled, so any delayed
// metadata is unusable: the type must be known while the row is built.
//
// This module is a pure, header-only, FFmpeg-free bit reader. It only needs
// the first few bits of a slice header, so it never decodes pixels and never
// holds state between packets. All reads are bounds-checked: malformed or
// truncated packets simply return nullopt instead of reading out of bounds.
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

namespace ma::infra {

// The caller maps AVCodecID to one of these; keeping FFmpeg types out makes
// this unit-testable with synthetic NAL bytes and no FFmpeg linkage.
enum class CodecFamily { H264, Hevc, Mpeg12, IntraOnly, Unknown };

namespace slice_detail {

struct BitReader {
    const uint8_t* p = nullptr;
    size_t n = 0;
    size_t bit = 0;
    bool ok = true;

    uint32_t u(int count) {
        uint32_t v = 0;
        for (int i = 0; i < count; ++i) {
            const size_t byte = bit >> 3;
            if (byte >= n) { ok = false; return 0; }
            v = (v << 1) |
                static_cast<uint32_t>((p[byte] >> (7 - (bit & 7))) & 1u);
            ++bit;
        }
        return v;
    }

    // Exp-Golomb ue(v). Leading zeros count is capped so a garbage buffer
    // can never spin or shift into undefined behaviour.
    uint32_t ue() {
        int lz = 0;
        while (lz < 32) {
            const uint32_t b = u(1);
            if (!ok) return 0;
            if (b) break;
            ++lz;
        }
        if (lz >= 32) { ok = false; return 0; }
        if (lz == 0) return 0;
        return ((static_cast<uint32_t>(1) << lz) - 1u) + u(lz);
    }
};

// Strip emulation-prevention bytes from a short RBSP prefix. Only the first
// bytes are needed (first_mb_in_slice + slice_type fit well within 16 bytes).
inline size_t toRbsp(const uint8_t* src, size_t n, uint8_t* dst, size_t cap) {
    size_t w = 0;
    int zeros = 0;
    for (size_t i = 0; i < n && w < cap; ++i) {
        const uint8_t b = src[i];
        if (zeros >= 2 && b == 0x03 && i + 1 < n && src[i + 1] <= 0x03) {
            zeros = 0;
            continue;
        }
        dst[w++] = b;
        zeros = (b == 0) ? zeros + 1 : 0;
    }
    return w;
}

struct NalSpan {
    const uint8_t* p = nullptr;
    size_t n = 0;
};

// Split into NAL units. Handles Annex-B start codes (TS / RTP / raw H.264)
// and length-prefixed avcC/hvcC (MP4 / MOV), auto-detecting the prefix size.
inline std::vector<NalSpan> splitNals(const uint8_t* data, size_t size,
                                      size_t maxNals) {
    std::vector<NalSpan> out;
    if (!data || size < 2 || maxNals == 0) return out;

    const bool annexB =
        (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
        (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 &&
         data[3] == 1);
    if (annexB) {
        std::vector<size_t> starts;
        for (size_t i = 0; i + 2 < size; ++i) {
            if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
                starts.push_back(i + 3);
                i += 2;
            }
        }
        for (size_t k = 0; k < starts.size() && out.size() < maxNals; ++k) {
            const size_t s = starts[k];
            size_t e = (k + 1 < starts.size()) ? starts[k + 1] - 3 : size;
            while (e > s && data[e - 1] == 0) --e;  // trailing zeros of next start code
            if (e > s) out.push_back({data + s, e - s});
        }
        return out;
    }

    // Length-prefixed: accept only a layout that consumes the whole packet and
    // whose NAL headers have forbidden_zero_bit == 0. Rejects raw garbage.
    for (const int ls : {4, 3, 2, 1}) {
        std::vector<NalSpan> cand;
        size_t pos = 0;
        bool ok = true;
        while (pos + static_cast<size_t>(ls) <= size &&
               cand.size() < maxNals) {
            uint32_t len = 0;
            for (int b = 0; b < ls; ++b)
                len = (len << 8) |
                      static_cast<uint32_t>(data[pos + static_cast<size_t>(b)]);
            pos += static_cast<size_t>(ls);
            if (len == 0 || len > size - pos) { ok = false; break; }
            if ((data[pos] & 0x80u) == 0)
                cand.push_back({data + pos, static_cast<size_t>(len)});
            pos += len;
        }
        if (ok && pos == size && !cand.empty()) return cand;
    }
    return out;
}

// H.264 slice_header: first_mb_in_slice ue(v), slice_type ue(v).
// slice_type 0..4 = P,B,I,SP,SI; 5..9 are the same "all slices" variants.
inline std::optional<std::string> h264SliceType(const uint8_t* nal, size_t n) {
    if (!nal || n < 2) return std::nullopt;
    const int type = static_cast<int>(nal[0] & 0x1fu);
    if (type != 1 && type != 5 && type != 19) return std::nullopt;  // VCL slices
    uint8_t rbsp[16] = {};
    const size_t take = (n - 1 < sizeof(rbsp)) ? n - 1 : sizeof(rbsp);
    BitReader br{rbsp, toRbsp(nal + 1, take, rbsp, sizeof(rbsp)), 0, true};
    (void)br.ue();                 // first_mb_in_slice
    const uint32_t st = br.ue();   // slice_type
    if (!br.ok || st > 9) return std::nullopt;
    switch (st % 5) {
    case 0: return std::string("P");
    case 1: return std::string("B");
    case 2: return std::string("I");
    case 3: return std::string("P");  // SP
    case 4: return std::string("I");  // SI
    }
    return std::nullopt;
}

// HEVC slice_segment_header: first_slice_segment_in_pic_flag u(1),
// [no_output_of_prior_pics_flag u(1) for IRAP NALs],
// slice_pic_parameter_set_id ue(v), [extra PPS bits], slice_type ue(v) 0=B,1=P,2=I.
// Dependent slices need PPS state we deliberately do not keep, so they fall
// back to the caller's keyframe marker instead of guessing.
inline std::optional<std::string> hevcSliceType(const uint8_t* nal, size_t n) {
    if (!nal || n < 3) return std::nullopt;
    const int type = static_cast<int>((nal[0] >> 1) & 0x3fu);
    if (type > 31) return std::nullopt;  // non-VCL NAL (VPS/SPS/PPS/SEI)
    uint8_t rbsp[16] = {};
    const size_t take = (n - 2 < sizeof(rbsp)) ? n - 2 : sizeof(rbsp);
    BitReader br{rbsp, toRbsp(nal + 2, take, rbsp, sizeof(rbsp)), 0, true};
    const uint32_t first = br.u(1);
    if (type >= 16 && type <= 23) (void)br.u(1);
    if (!br.ok) return std::nullopt;
    if (first == 0) return std::nullopt;
    (void)br.ue();                 // slice_pic_parameter_set_id
    const uint32_t st = br.ue();   // slice_type
    if (!br.ok || st > 2) return std::nullopt;
    return std::string(st == 0 ? "B" : (st == 1 ? "P" : "I"));
}

// MPEG-1/2 picture header after the 00 00 01 00 picture start code:
// temporal_reference u(10), picture_coding_type u(3): 1=I, 2=P, 3=B, 4=D.
inline std::optional<std::string> mpegSliceType(const uint8_t* data,
                                                size_t size) {
    if (!data || size < 6) return std::nullopt;
    for (size_t i = 0; i + 5 < size; ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1 &&
            data[i + 3] == 0x00) {
            const size_t avail = size - i - 4;
            BitReader br{data + i + 4, avail < 4 ? avail : 4, 0, true};
            (void)br.u(10);
            const uint32_t t = br.u(3);
            if (!br.ok) return std::nullopt;
            if (t == 1) return std::string("I");
            if (t == 2) return std::string("P");
            if (t == 3) return std::string("B");
            return std::nullopt;
        }
    }
    return std::nullopt;
}

}  // namespace slice_detail

// Returns "I" / "P" / "B" (or nullopt when the bitstream cannot be identified).
// The caller should still honour AV_PKT_FLAG_KEY as an authoritative override.
inline std::optional<std::string> probeSliceType(CodecFamily family,
                                                 const uint8_t* data,
                                                 size_t size) {
    if (!data || size == 0) return std::nullopt;
    if (family == CodecFamily::IntraOnly) return std::string("I");
    if (family == CodecFamily::Mpeg12)
        return slice_detail::mpegSliceType(data, size);
    if (family != CodecFamily::H264 && family != CodecFamily::Hevc)
        return std::nullopt;

    const auto nals = slice_detail::splitNals(data, size, 32);
    for (const auto& nal : nals) {
        auto t = (family == CodecFamily::H264)
                     ? slice_detail::h264SliceType(nal.p, nal.n)
                     : slice_detail::hevcSliceType(nal.p, nal.n);
        if (t) return t;
    }
    return std::nullopt;
}

}  // namespace ma::infra
