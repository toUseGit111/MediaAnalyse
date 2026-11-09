// Unit test for the synchronous I/P/B bitstream reader (SliceType.hpp).
// Header-only + standard library only: no FFmpeg linkage, no media files.
// Synthetic NAL units exercise Annex-B / avcC / hvcC layouts, H.264 slice_type,
// HEVC slice_type, MPEG-2 picture_coding_type, intra-only codecs, and a
// deterministic fuzz sweep for malformed/truncated packets.
#include "ma/infra/SliceType.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using ma::infra::CodecFamily;
using ma::infra::probeSliceType;
using Bytes = std::vector<uint8_t>;

static int gFailed = 0;
static int gTotal = 0;

static void expect(const char* name, bool ok, const std::string& detail = {}) {
    ++gTotal;
    if (!ok) ++gFailed;
    std::printf("%s  %-42s %s\n", ok ? "PASS" : "FAIL", name,
                detail.c_str());
}

static std::string typeOf(CodecFamily fam, const Bytes& b) {
    auto t = probeSliceType(fam, b.empty() ? nullptr : b.data(), b.size());
    return t ? *t : std::string("-");
}

// H.264 slice RBSP: first_mb_in_slice=0 (ue '1'), slice_type, pps_id=0 ('1').
//   P (ue0='1')    -> 1 1 1 00000 -> 0xE0
//   I (ue2='011')  -> 1 011 1 000 -> 0xB8
//   B (ue1='010')  -> 1 010 1 000 -> 0xA8
static Bytes h264AnnexB(uint8_t nalHeader, uint8_t sliceBits) {
    return Bytes{0x00, 0x00, 0x00, 0x01, nalHeader, sliceBits, 0x00};
}

int main() {
    // ---- H.264 Annex-B (TS / RTP / raw) -----------------------------------
    expect("H264 annexb IDR slice -> I",
           typeOf(CodecFamily::H264, h264AnnexB(0x65, 0xB8)) == "I");
    expect("H264 annexb non-IDR slice_type=0 -> P",
           typeOf(CodecFamily::H264, h264AnnexB(0x41, 0xE0)) == "P");
    expect("H264 annexb non-IDR slice_type=1 -> B",
           typeOf(CodecFamily::H264, h264AnnexB(0x41, 0xA8)) == "B");

    // Non-VCL NALs (SPS/PPS/SEI/AUD) must be skipped, not misread as a slice.
    Bytes withSps{0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1E,
                  0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80,
                  0x00, 0x00, 0x01, 0x41, 0xE0, 0x00};
    expect("H264 skips SPS/PPS, finds P slice",
           typeOf(CodecFamily::H264, withSps) == "P");

    // ---- avcC / hvcC length-prefixed (MP4 / MOV) --------------------------
    Bytes avcc4{0x00, 0x00, 0x00, 0x03, 0x67, 0x42, 0x00,
                0x00, 0x00, 0x00, 0x02, 0x41, 0xE0};
    expect("H264 avcC 4-byte length -> P",
           typeOf(CodecFamily::H264, avcc4) == "P");
    Bytes avcc2{0x00, 0x03, 0x67, 0x42, 0x00,
                0x00, 0x02, 0x65, 0xB8};
    expect("H264 avcC 2-byte length -> I",
           typeOf(CodecFamily::H264, avcc2) == "I");
    // A packet with no VCL slice at all must not fabricate a type.
    Bytes headersOnly{0x00, 0x00, 0x00, 0x02, 0x67, 0x42};
    expect("H264 header-only packet -> unknown",
           typeOf(CodecFamily::H264, headersOnly) == "-");

    // ---- HEVC -------------------------------------------------------------
    // nal[0]=(type<<1), nal[1]=temporal_id_plus1.
    // IDR_W_RADL(19): first=1, no_output=0, pps ue0='1', slice_type ue2='011'
    //   -> 1 0 1 011 0000 -> 0xAC 0x00
    // TRAIL_R(1): first=1, pps ue0='1', slice_type ue1='010'
    //   -> 1 1 010 0000 -> 0xD0 0x00  (P)
    // TRAIL_R with slice_type ue0='1' -> 1 1 1 00000 -> 0xE0 0x00 (B)
    Bytes hevcIdr{0x00, 0x00, 0x00, 0x01, 0x26, 0x01, 0xAC, 0x00};
    expect("HEVC IDR slice_type=2 -> I",
           typeOf(CodecFamily::Hevc, hevcIdr) == "I");
    Bytes hevcP{0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0xD0, 0x00};
    expect("HEVC TRAIL_R slice_type=1 -> P",
           typeOf(CodecFamily::Hevc, hevcP) == "P");
    Bytes hevcB{0x00, 0x00, 0x00, 0x01, 0x02, 0x01, 0xE0, 0x00};
    expect("HEVC TRAIL_R slice_type=0 -> B",
           typeOf(CodecFamily::Hevc, hevcB) == "B");
    Bytes hevcVps{0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01};
    expect("HEVC non-VCL NAL -> unknown",
           typeOf(CodecFamily::Hevc, hevcVps) == "-");

    // ---- MPEG-1/2 ---------------------------------------------------------
    // 00 00 01 00 = picture_start_code; then u(10) temporal_reference,
    // u(3) picture_coding_type. byte1 bit layout: 00 (temporal tail) + type.
    expect("MPEG2 picture_coding_type=2 -> P",
           typeOf(CodecFamily::Mpeg12,
                  Bytes{0x00, 0x00, 0x01, 0x00, 0x00, 0x10}) == "P");
    expect("MPEG2 picture_coding_type=1 -> I",
           typeOf(CodecFamily::Mpeg12,
                  Bytes{0x00, 0x00, 0x01, 0x00, 0x00, 0x08}) == "I");
    expect("MPEG2 picture_coding_type=3 -> B",
           typeOf(CodecFamily::Mpeg12,
                  Bytes{0x00, 0x00, 0x01, 0x00, 0x00, 0x18}) == "B");

    // ---- intra-only / unsupported / empty ---------------------------------
    expect("intra-only codec -> I",
           typeOf(CodecFamily::IntraOnly, Bytes{0x01}) == "I");
    expect("unsupported codec -> unknown",
           typeOf(CodecFamily::Unknown, h264AnnexB(0x65, 0xB8)) == "-");
    expect("empty packet -> unknown",
           typeOf(CodecFamily::H264, Bytes{}) == "-");
    expect("truncated slice -> unknown",
           typeOf(CodecFamily::H264, Bytes{0x00, 0x00, 0x01, 0x65}) == "-");
    expect("garbage -> unknown",
           typeOf(CodecFamily::H264,
                  Bytes{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}) == "-");

    // ---- RBSP emulation-prevention handling -------------------------------
    {
        const uint8_t src[] = {0x00, 0x00, 0x03, 0x01, 0x1E};
        uint8_t dst[8] = {};
        const size_t n = ma::infra::slice_detail::toRbsp(src, 5, dst, 8);
        expect("RBSP strips emulation-prevention byte",
               n == 4 && dst[0] == 0 && dst[1] == 0 && dst[2] == 1 &&
                   dst[3] == 0x1E);
    }

    // ---- deterministic malformed-input sweep -------------------------------
    // Guards the bit reader / NAL splitter against out-of-bounds reads. The
    // worker also runs fuzz_worker.py on the full binary; this is the fast gate.
    uint32_t rng = 0x13579BDFu;
    int sweep = 0;
    bool sweepOk = true;
    for (int i = 0; i < 4000; ++i) {
        rng ^= rng << 13;
        rng ^= rng >> 17;
        rng ^= rng << 5;
        const size_t n = static_cast<size_t>(rng % 65u);
        Bytes b(n);
        for (size_t j = 0; j < n; ++j) {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            b[j] = static_cast<uint8_t>(rng & 0xFFu);
        }
        for (CodecFamily fam : {CodecFamily::H264, CodecFamily::Hevc,
                                CodecFamily::Mpeg12, CodecFamily::Unknown}) {
            const auto t = probeSliceType(
                fam, b.empty() ? nullptr : b.data(), b.size());
            if (t && *t != "I" && *t != "P" && *t != "B") {
                sweepOk = false;
                break;
            }
            ++sweep;
        }
        if (!sweepOk) break;
    }
    expect("4000 malformed packets: no crash, valid labels only",
           sweepOk, std::to_string(sweep) + " probes");

    std::printf("\n%s (%d checks, %d failed)\n",
                gFailed ? "FAILED" : "ALL PASS", gTotal, gFailed);
    return gFailed ? 1 : 0;
}
