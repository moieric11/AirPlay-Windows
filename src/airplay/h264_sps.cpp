#include "airplay/h264_sps.h"

#include <cstdint>
#include <vector>

namespace ap::airplay {
namespace {

// --- RBSP extraction -------------------------------------------------------
//
// H.264 NAL units are encoded with "emulation prevention bytes": any
// occurrence of 0x00 0x00 0x00, 0x00 0x00 0x01, 0x00 0x00 0x02 or
// 0x00 0x00 0x03 inside the bitstream gets escaped by inserting 0x03 after
// the 0x00 0x00. Before bit-level parsing we reverse that substitution.
std::vector<uint8_t> ebsp_to_rbsp(const uint8_t* p, std::size_t n) {
    std::vector<uint8_t> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        if (i + 2 < n && p[i] == 0x00 && p[i + 1] == 0x00 && p[i + 2] == 0x03) {
            out.push_back(0x00);
            out.push_back(0x00);
            i += 2;
        } else {
            out.push_back(p[i]);
        }
    }
    return out;
}

// --- Bit reader with Exp-Golomb (unsigned / signed) ------------------------
struct BitReader {
    const uint8_t* buf;
    std::size_t    len;
    std::size_t    pos = 0;
    bool           error = false;

    bool u1() {
        if (pos >= len * 8) { error = true; return false; }
        uint8_t byte = buf[pos >> 3];
        bool bit = (byte >> (7 - (pos & 7))) & 1;
        ++pos;
        return bit;
    }

    uint32_t u(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) {
            v = (v << 1) | (u1() ? 1u : 0u);
            if (error) return 0;
        }
        return v;
    }

    // Unsigned Exp-Golomb: count leading zeros, then read that many bits +1.
    uint32_t ue() {
        int zeros = 0;
        while (!u1() && !error && zeros < 32) ++zeros;
        if (error) return 0;
        if (zeros == 0) return 0;
        uint32_t v = u(zeros);
        if (error) return 0;
        return (1u << zeros) - 1 + v;
    }

    int32_t se() {
        uint32_t k = ue();
        if (error) return 0;
        // Mapping per H.264 9.1.1.
        if (k == 0) return 0;
        return (k & 1) ? static_cast<int32_t>((k + 1) / 2)
                       : -static_cast<int32_t>(k / 2);
    }
};

// Frame-size math from the SPS fields (H.264 7.4.2.1).
void compute_frame_size(uint32_t pic_width_in_mbs_minus1,
                        uint32_t pic_height_in_map_units_minus1,
                        bool     frame_mbs_only_flag,
                        bool     cropping,
                        uint32_t crop_left,  uint32_t crop_right,
                        uint32_t crop_top,   uint32_t crop_bottom,
                        uint32_t& out_w, uint32_t& out_h) {
    uint32_t w_mb = (pic_width_in_mbs_minus1 + 1) * 16;
    uint32_t h_mb = (pic_height_in_map_units_minus1 + 1) * 16;
    if (!frame_mbs_only_flag) h_mb *= 2;

    uint32_t crop_x = 2 * (crop_left + crop_right);
    uint32_t crop_y = 2 * (crop_top + crop_bottom) * (frame_mbs_only_flag ? 1 : 2);
    if (!cropping) { crop_x = 0; crop_y = 0; }

    out_w = (w_mb > crop_x) ? w_mb - crop_x : w_mb;
    out_h = (h_mb > crop_y) ? h_mb - crop_y : h_mb;
}

} // namespace

bool parse_h264_sps(const uint8_t* sps, std::size_t len, SpsInfo& out) {
    out = {};
    if (!sps || len < 4) return false;

    // Skip the NAL header byte (index 0). Its low 5 bits should be 7 (SPS).
    if ((sps[0] & 0x1f) != 7) return false;

    auto rbsp = ebsp_to_rbsp(sps + 1, len - 1);
    if (rbsp.size() < 3) return false;

    // Per H.264 7.3.2.1.1 the SPS starts with:
    //   profile_idc (8), constraint_set_flags (8), level_idc (8), then ue/se fields.
    out.profile_idc = rbsp[0];
    // rbsp[1] = constraint_set_flags + 2 reserved zero bits
    out.level_idc   = rbsp[2];

    BitReader br{ rbsp.data() + 3, rbsp.size() - 3 };

    /*uint32_t seq_parameter_set_id =*/ br.ue();

    // Some high/extended profiles include chroma/bit-depth extensions.
    const uint8_t p = out.profile_idc;
    const bool has_chroma_ext = (p == 100 || p == 110 || p == 122 || p == 244 ||
                                 p ==  44 || p ==  83 || p ==  86 || p == 118 ||
                                 p == 128 || p == 138 || p == 139 || p == 134 ||
                                 p == 135);
    if (has_chroma_ext) {
        uint32_t chroma_format_idc = br.ue();
        if (chroma_format_idc == 3) br.u1();                  // separate_colour_plane_flag
        /*uint32_t bit_depth_luma =*/   br.ue();
        /*uint32_t bit_depth_chroma =*/ br.ue();
        br.u1();                                              // qpprime_y_zero_transform_bypass_flag
        if (br.u1()) {                                        // seq_scaling_matrix_present_flag
            int count = (chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < count && !br.error; ++i) {
                if (br.u1()) {                                // seq_scaling_list_present_flag[i]
                    int size = (i < 6) ? 16 : 64;
                    int last = 8, next = 8;
                    for (int j = 0; j < size && !br.error; ++j) {
                        if (next) {
                            int delta = br.se();
                            next = (last + delta + 256) % 256;
                        }
                        last = next ? next : last;
                    }
                }
            }
        }
    }

    /*uint32_t log2_max_frame_num_minus4 =*/ br.ue();
    uint32_t pic_order_cnt_type = br.ue();
    if (pic_order_cnt_type == 0) {
        /*log2_max_pic_order_cnt_lsb_minus4 =*/ br.ue();
    } else if (pic_order_cnt_type == 1) {
        br.u1();                            // delta_pic_order_always_zero_flag
        br.se();                            // offset_for_non_ref_pic
        br.se();                            // offset_for_top_to_bottom_field
        uint32_t n = br.ue();               // num_ref_frames_in_pic_order_cnt_cycle
        for (uint32_t i = 0; i < n && !br.error; ++i) br.se();
    }

    /*uint32_t num_ref_frames =*/           br.ue();
    /*bool gaps_in_frame_num_value_allowed =*/ br.u1();
    uint32_t pic_width_in_mbs_minus1        = br.ue();
    uint32_t pic_height_in_map_units_minus1 = br.ue();
    bool     frame_mbs_only_flag            = br.u1();
    if (!frame_mbs_only_flag) {
        br.u1();                            // mb_adaptive_frame_field_flag
    }
    /*bool direct_8x8_inference_flag =*/    br.u1();
    bool frame_cropping_flag                = br.u1();
    uint32_t crop_left = 0, crop_right = 0, crop_top = 0, crop_bottom = 0;
    if (frame_cropping_flag) {
        crop_left   = br.ue();
        crop_right  = br.ue();
        crop_top    = br.ue();
        crop_bottom = br.ue();
    }

    if (br.error) return false;

    compute_frame_size(pic_width_in_mbs_minus1, pic_height_in_map_units_minus1,
                       frame_mbs_only_flag,
                       frame_cropping_flag,
                       crop_left, crop_right, crop_top, crop_bottom,
                       out.width, out.height);
    out.interlaced = !frame_mbs_only_flag;
    return true;
}

std::string profile_name(uint8_t p) {
    switch (p) {
        case  66: return "Baseline";
        case  77: return "Main";
        case  88: return "Extended";
        case 100: return "High";
        case 110: return "High 10";
        case 122: return "High 4:2:2";
        case 244: return "High 4:4:4";
        case  44: return "CAVLC 4:4:4";
        default:  return "Profile " + std::to_string(p);
    }
}

} // namespace ap::airplay
