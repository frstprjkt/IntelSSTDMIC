// Types shared by the generated IPC replay tables (IpcReplay.hpp = 4 ch, IpcReplay2ch.hpp = 2 ch).
#pragma once
#include <stdint.h>

struct IpcMsg { uint32_t cmd; uint16_t words; uint16_t patch; const uint32_t *data; const char *name; };
enum : uint16_t { kPatchNone = 0, kPatchPcmParams = 1 };

// STREAM_PCM_PARAMS layout (word index into the full message incl. 8-byte header):
//  [2] comp_id (host PCM6C = 0x32)   [8] buffer.phy_addr (page table, <4 GB)
//  [9] buffer.pages                  [10] buffer.size (ring bytes)
//  [14] direction (1 = capture)       [15] frame_fmt (2 = S32_LE)  [17] rate
//  [18] stream_tag (lo16) | channels (hi16)   [20] host_period_bytes
enum { kPcmParamPhy = 8, kPcmParamPages = 9, kPcmParamSize = 10, kPcmParamTagCh = 18, kPcmParamPeriod = 20 };

// One complete DMIC pipeline: what replay() sends for setup / start / stop (+ pcm free) / teardown.
struct IpcPipeline {
    const IpcMsg *setup; uint32_t setupCount;
    const IpcMsg *start; uint32_t startCount;
    const IpcMsg *stop; uint32_t stopCount;          // [0] TRIG_STOP, [1..] PCM_FREE
    const IpcMsg *teardown; uint32_t teardownCount;
    uint32_t channels;                               // PCM channel count the pipeline was captured with
    const char *source;                              // machine the capture came from
};
