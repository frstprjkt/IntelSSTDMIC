// Layout configuration.
//
// A layout is the single configuration entity for a DSP microphone: DSP platform/engine class,
// firmware identity, microphone geometry, HDA stream identifiers and the supported HDA formats.
// Every entry in Layouts/Layouts.plist is compiled in at build time (embedded XML); the same data is
// copied into the kext Info.plist under IntelSSTDMLayouts. There is no runtime plist dependency.
#pragma once
#include <stdint.h>


enum class PlatformFamily : uint32_t {
    Unknown = 0,
    TigerLake = 1,
    CometLake = 2,
    AlderLake = 3,
};

struct LayoutConfig {
    bool valid {false};
    uint32_t id {0};
    char name[64] {};

    PlatformFamily platform {PlatformFamily::Unknown};
    char engine[24] {};             // factory selector: "tgl", "cml", "adl"

    uint32_t micCount {2};
    uint32_t codecCount {0};
    char codecs[4][16] {};

    // DSP host path (firmware / IPC / ring)
    char firmware[64] {};
    uint32_t streamIndex {6};
    uint32_t pcmTag {7};
    uint32_t pipelineId {57};
    uint32_t sourceRate {48000};
    uint32_t sourceChannels {4};
    uint32_t sourceBits {32};
    uint32_t gainPercent {100};

    // HDA engine geometry / advertised formats
    uint32_t outputChannels {2};
    uint32_t channelMap[8] {0, 1, 2, 3};
    uint32_t safetyFrames {57};
    uint32_t sampleRates[8] {48000};
    uint32_t sampleRateCount {1};
    uint32_t bitDepths[8] {16, 24, 32};
    uint32_t bitDepthCount {3};

    uint32_t keepAliveSec {60};
    bool allowD0i3 {false};
};

class LayoutRegistry {
public:
    LayoutRegistry();

    const LayoutConfig *find(uint32_t id) const;
    const LayoutConfig *findByName(const char *name) const;
    const LayoutConfig *defaultFor(PlatformFamily platform) const;
    const LayoutConfig *at(uint32_t index) const;
    uint32_t count() const { return layoutCount; }
    bool addLayout(const LayoutConfig &layout) { return add(layout); }

    // layout id -> named layout -> model substring -> platform default -> invalid
    const LayoutConfig *resolve(int32_t layoutId, const char *layoutName,
                                const char *model, PlatformFamily platform) const;

    static const LayoutConfig &none();

private:
    static constexpr uint32_t kMaxLayouts = 16;
    LayoutConfig layouts[kMaxLayouts] {};
    uint32_t layoutCount {0};

    bool add(const LayoutConfig &layout);
};
