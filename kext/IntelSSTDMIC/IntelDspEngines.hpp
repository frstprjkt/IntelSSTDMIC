// Concrete cAVS/SOF DSP engines. Platform differences only: identity, core mask and firmware image.
#pragma once
#include "IntelDspAudioEngine.hpp"

class TglDspAudioEngine : public IntelDspAudioEngine {
public:
    const char *platformName() const override { return "Tiger Lake (cAVS 2.5)"; }
    PlatformFamily platform() const override { return PlatformFamily::TigerLake; }
    const char *engineName() const override { return "TglDspAudioEngine"; }
    uint32_t defaultCaptureIndex() const override { return 6; }
    uint32_t defaultCaptureTag() const override { return 7; }

protected:
    const uint8_t *firmwareImage(uint32_t &size) const override;
    uint32_t targetCoreMask() const override;
};

class CmlDspAudioEngine : public IntelDspAudioEngine {
public:
    const char *platformName() const override { return "Comet Lake / Cannon Lake (cAVS 1.8)"; }
    PlatformFamily platform() const override { return PlatformFamily::CometLake; }
    const char *engineName() const override { return "CmlDspAudioEngine"; }
    uint32_t defaultCaptureIndex() const override { return 6; }
    uint32_t defaultCaptureTag() const override { return 7; }

protected:
    const uint8_t *firmwareImage(uint32_t &size) const override;
    uint32_t targetCoreMask() const override;
};
