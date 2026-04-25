#include "airplay/h264_nal.h"

namespace ap::airplay {

bool parse_nals_avcc_to_annexb(unsigned char* p, std::size_t size,
                               std::vector<NalUnit>& out,
                               NalCodec codec) {
    out.clear();
    std::size_t pos = 0;
    while (pos < size) {
        if (pos + 4 > size) return false;

        // Big-endian 32-bit NAL length (AVCC format, ISO 14496-15).
        // iOS uses 4-byte length prefix for both H.264 and HEVC mirror.
        uint32_t nal_len =  (static_cast<uint32_t>(p[pos + 0]) << 24)
                         | (static_cast<uint32_t>(p[pos + 1]) << 16)
                         | (static_cast<uint32_t>(p[pos + 2]) <<  8)
                         |  static_cast<uint32_t>(p[pos + 3]);
        if (nal_len == 0 || pos + 4 + nal_len > size) return false;

        // forbidden_zero_bit is bit 7 of byte 0 in BOTH codecs and
        // must be zero — a useful malformed-input guard.
        const unsigned char nh = p[pos + 4];
        if (nh & 0x80) return false;

        NalUnit u;
        if (codec == NalCodec::H264) {
            // byte 0: forbidden(1) | ref_idc(2) | type(5)
            u.ref_idc = (nh >> 5) & 0x03;
            u.type    =  nh       & 0x1f;
        } else {
            // HEVC byte 0: forbidden(1) | type(6) | layer_id_high(1)
            // (the 5 remaining layer_id + 3-bit temporal_id sit in byte 1
            //  and don't matter for our cosmetic NAL-type accounting).
            u.ref_idc = 0;
            u.type    = (nh >> 1) & 0x3f;
        }
        u.offset = pos + 4;
        u.size   = nal_len;
        out.push_back(u);

        // Overwrite the length prefix with the Annex-B 4-byte start code
        // so the payload can be fed straight into a decoder expecting
        // 00 00 00 01 before each NAL.
        p[pos + 0] = 0x00;
        p[pos + 1] = 0x00;
        p[pos + 2] = 0x00;
        p[pos + 3] = 0x01;

        pos += 4 + nal_len;
    }
    return true;
}

namespace {
std::string h264_type_name(int type) {
    switch (type) {
        case  1: return "NON_IDR_SLICE";
        case  2: return "SLICE_DPA";
        case  3: return "SLICE_DPB";
        case  4: return "SLICE_DPC";
        case  5: return "IDR_SLICE";
        case  6: return "SEI";
        case  7: return "SPS";
        case  8: return "PPS";
        case  9: return "AUD";
        case 10: return "END_OF_SEQUENCE";
        case 11: return "END_OF_STREAM";
        case 12: return "FILLER";
        case 13: return "SPS_EXT";
        case 14: return "PREFIX_NAL";
        case 15: return "SUBSET_SPS";
        case 19: return "AUX_SLICE";
        default: return "TYPE_" + std::to_string(type);
    }
}
std::string hevc_type_name(int type) {
    // HEVC NAL types per H.265 spec table 7-1. Only the ones iOS
    // mirror is likely to emit are spelled out — the rest fall
    // through to a generic TYPE_N for compactness.
    switch (type) {
        case  0: return "TRAIL_N";
        case  1: return "TRAIL_R";
        case  2: return "TSA_N";
        case  3: return "TSA_R";
        case  4: return "STSA_N";
        case  5: return "STSA_R";
        case  6: return "RADL_N";
        case  7: return "RADL_R";
        case  8: return "RASL_N";
        case  9: return "RASL_R";
        case 16: return "BLA_W_LP";
        case 17: return "BLA_W_RADL";
        case 18: return "BLA_N_LP";
        case 19: return "IDR_W_RADL";
        case 20: return "IDR_N_LP";
        case 21: return "CRA_NUT";
        case 32: return "VPS";
        case 33: return "SPS";
        case 34: return "PPS";
        case 35: return "AUD";
        case 36: return "EOS";
        case 37: return "EOB";
        case 38: return "FD";
        case 39: return "PREFIX_SEI";
        case 40: return "SUFFIX_SEI";
        default: return "TYPE_" + std::to_string(type);
    }
}
} // namespace

std::string nal_type_name(int type, NalCodec codec) {
    return codec == NalCodec::H264 ? h264_type_name(type)
                                   : hevc_type_name(type);
}

} // namespace ap::airplay
