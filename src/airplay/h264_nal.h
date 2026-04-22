#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ap::airplay {

// One H.264 NAL unit found inside a decrypted mirror frame payload.
struct NalUnit {
    int         type    = 0;   // nal_unit_type (5 bits), e.g. 5 = IDR, 1 = P-slice
    int         ref_idc = 0;   // nal_ref_idc   (2 bits)
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
// Returns false when the data is malformed (length would overrun the buffer,
// forbidden_zero_bit set on a NAL header). `out` is populated partially in
// that case for diagnostic purposes.
bool parse_nals_avcc_to_annexb(unsigned char* payload, std::size_t payload_size,
                               std::vector<NalUnit>& out);

// Human-readable name for common H.264 NAL types. Returns a generic
// "type N" string for unknown ones.
std::string nal_type_name(int type);

} // namespace ap::airplay
