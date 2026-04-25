#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ap::airplay {

// Which NAL header layout to use when interpreting the per-NAL
// metadata. iOS mirror swaps to HEVC when the receiver advertises
// a high-enough display resolution; the AVCC length-prefix wrapper
// is identical, but the inner NAL header is laid out differently:
//   H.264: 1 byte  — forbidden(1) | ref_idc(2) | type(5)
//   HEVC : 2 bytes — forbidden(1) | type(6)    | layer_id(6) | tid(3)
enum class NalCodec { H264, HEVC };

// One NAL unit found inside a decrypted mirror frame payload.
struct NalUnit {
    int         type    = 0;   // codec-specific nal_unit_type
    int         ref_idc = 0;   // H.264 only; 0 for HEVC NALs
    std::size_t offset  = 0;   // position of the NAL header byte inside the payload
    std::size_t size    = 0;   // NAL size in bytes, EXCLUDING the 4-byte start code
};

// Convert an AVCC-style length-prefixed NAL stream IN PLACE to Annex-B,
// overwriting each 4-byte big-endian length prefix with the start code
// `00 00 00 01` and reporting every NAL found via `out`.
//
// Format of the input payload (as emitted by iOS after AES-CTR decrypt):
//   [4B BE length][NAL bytes][4B BE length][NAL bytes]...
//
// `codec` selects the NAL-header layout used to populate NalUnit.type
// (and ref_idc for H.264). The byte rewriting is codec-agnostic.
//
// Returns false when the data is malformed (length would overrun the buffer,
// forbidden_zero_bit set on a NAL header). `out` is populated partially in
// that case for diagnostic purposes.
bool parse_nals_avcc_to_annexb(unsigned char* payload, std::size_t payload_size,
                               std::vector<NalUnit>& out,
                               NalCodec codec = NalCodec::H264);

// Human-readable name for common H.264 / HEVC NAL types. Returns a
// generic "TYPE_N" string for unknown ones. The codec parameter is
// required because the same numeric type means different things in
// each codec (e.g. type=5 → IDR_SLICE in H.264 but TRAIL_R in HEVC).
std::string nal_type_name(int type, NalCodec codec = NalCodec::H264);

} // namespace ap::airplay
