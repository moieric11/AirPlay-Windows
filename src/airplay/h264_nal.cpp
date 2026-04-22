#include "airplay/h264_nal.h"

namespace ap::airplay {

bool parse_nals_avcc_to_annexb(unsigned char* p, std::size_t size,
                               std::vector<NalUnit>& out) {
    out.clear();
    std::size_t pos = 0;
    while (pos < size) {
        if (pos + 4 > size) return false;

        // Big-endian 32-bit NAL length (H.264 AVCC format, ISO 14496-15).
        uint32_t nal_len =  (static_cast<uint32_t>(p[pos + 0]) << 24)
                         | (static_cast<uint32_t>(p[pos + 1]) << 16)
                         | (static_cast<uint32_t>(p[pos + 2]) <<  8)
                         |  static_cast<uint32_t>(p[pos + 3]);
        if (nal_len == 0 || pos + 4 + nal_len > size) return false;

        // First NAL byte: forbidden_zero_bit (1) | nal_ref_idc (2) | nal_unit_type (5)
        unsigned char nh = p[pos + 4];
        if (nh & 0x80) return false; // H.264 mandates forbidden_zero_bit == 0.

        NalUnit u;
        u.ref_idc = (nh >> 5) & 0x03;
        u.type    =  nh       & 0x1f;
        u.offset  = pos + 4;
        u.size    = nal_len;
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

std::string nal_type_name(int type) {
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

} // namespace ap::airplay
