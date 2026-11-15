// Minimal, bounds-checked NHLT (Non HD Audio Link Table) reader: number of PDM microphones.
// Mirrors Linux sound/hda/intel-nhlt.c intel_nhlt_get_dmic_geo(). Header-only and free of kernel
// dependencies so it can be unit-tested on the host (kext/tests/nhlt_test.cpp).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace Nhlt {
constexpr size_t kHeaderLen = 36;                // ACPI SDT header; endpoint count byte follows
constexpr uint8_t kLinkPdm = 2, kDirCapture = 1;
constexpr uint8_t kConfigTypeMicArray = 1;
enum : uint8_t {
    kArraySmallLinear2Ch = 0x0A, kArrayBigLinear2Ch = 0x0B, kArrayLinear4ChGeo1 = 0x0C,
    kArrayPlanarL4Ch = 0x0D, kArrayLinear4ChGeo2 = 0x0E, kArrayVendor = 0x0F,
};
// Endpoint descriptor: length(4) link_type(1) instance(1) vid(2) did(2) rev(2) subsys(4)
// device_type(1) direction(1) virtual_bus(1) = 19 bytes, then specific config {size(4), caps[size]}.
constexpr size_t kEpFixed = 19;
// Format: WAVEFORMATEXTENSIBLE (40 bytes) + specific config {size(4), caps[size]}; nChannels at +2.
constexpr size_t kFmtFixed = 40, kFmtChannels = 2;

inline uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// Result of the lookup; mics == 0 means "no PDM capture endpoint / unreadable table".
struct DmicInfo { uint32_t mics; uint8_t arrayType; bool fromFormats; };

// Highest DMIC count over all PDM capture endpoints. For a mic-array config the array type gives the
// count (vendor-defined: nb_mics); otherwise the largest channel count among the endpoint's formats.
inline DmicInfo dmicInfo(const uint8_t *t, size_t len) {
    DmicInfo r {0, 0, false};
    if (!t || len < kHeaderLen + 1 || t[0] != 'N' || t[1] != 'H' || t[2] != 'L' || t[3] != 'T') return r;
    size_t tblLen = rd32(t + 4);
    if (tblLen < kHeaderLen + 1 || tblLen > len) tblLen = len;
    size_t off = kHeaderLen + 1;
    for (uint32_t e = 0, n = t[kHeaderLen]; e < n; e++) {
        if (off + kEpFixed + 4 > tblLen) break;
        const uint8_t *ep = t + off;
        uint32_t epLen = rd32(ep);
        if (epLen < kEpFixed + 4 || off + epLen > tblLen) break;
        uint32_t cfgSize = rd32(ep + kEpFixed);
        const uint8_t *caps = ep + kEpFixed + 4;
        if (kEpFixed + 4 + (size_t)cfgSize > epLen) break;
        if (ep[4] == kLinkPdm && ep[17] == kDirCapture) {
            uint32_t mics = 0; uint8_t at = 0; bool fromFmt = false;
            // nhlt_dmic_array_config: caps[0] virtual_slot, [1] config_type, [2] array_type,
            // vendor variant: [3] nb_mics
            if (cfgSize >= 3 && caps[1] == kConfigTypeMicArray) {
                at = caps[2];
                switch (at) {
                    case kArraySmallLinear2Ch: case kArrayBigLinear2Ch: mics = 2; break;
                    case kArrayLinear4ChGeo1: case kArrayPlanarL4Ch: case kArrayLinear4ChGeo2: mics = 4; break;
                    case kArrayVendor: if (cfgSize >= 4) mics = caps[3]; break;
                    default: break;
                }
            }
            if (!mics) {                                // generic config: use the formats' channel count
                size_t fo = kEpFixed + 4 + cfgSize;
                if (fo + 1 <= epLen) {
                    uint32_t nf = ep[fo++];
                    for (uint32_t f = 0; f < nf && fo + kFmtFixed + 4 <= epLen; f++) {
                        uint16_t ch = rd16(ep + fo + kFmtChannels);
                        if (ch > mics) mics = ch;
                        uint32_t fcs = rd32(ep + fo + kFmtFixed);
                        if (fo + kFmtFixed + 4 + (size_t)fcs > epLen) break;
                        fo += kFmtFixed + 4 + fcs;
                    }
                    fromFmt = mics != 0;
                }
            }
            if (mics > r.mics) r = {mics, at, fromFmt};
        }
        off += epLen;
    }
    return r;
}
}  // namespace Nhlt
