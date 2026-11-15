// Default (native) HDA path plus the HDA hardware access shared by DSP engines.
//
// Default hook methods call the original AppleHDA functions captured in NativeHdaOps. IntelDspAudioEngine
// derives from this class, overrides the hooks it needs, and calls the base for pass-through.
// The Manager's native fallback only uses the hook defaults; it never attaches hardware.
#pragma once
#include "IAudioEngineHooks.hpp"
#include "NativeHdaOps.hpp"

class IOPCIDevice;
class IOMemoryMap;

class HdaAudioEngine : public IAudioEngineHooks {
public:
    HdaAudioEngine() = default;
    ~HdaAudioEngine() override;

    void setNativeOps(const NativeHdaOps *ops) override { nativeOps = ops; }
    const NativeHdaOps *native() const { return nativeOps; }

    // IAudioEngineHooks: native AppleHDA behaviour.
    bool initHardware(IOService *self, IOService *provider) override;
    IOReturn performAudioEngineStart(IOService *self) override;
    IOReturn performAudioEngineStop(IOService *self) override;
    void stopHardware(IOService *self, IOService *provider) override;
    IOReturn performFormatChange(IOService *self, IOAudioStream *stream,
                                 const IOAudioStreamFormat *format,
                                 const IOAudioSampleRate *rate) override;
    IOReturn convertInputSamples(IOService *self, const void *sampleBuf, void *destBuf,
                                 UInt32 firstFrame, UInt32 numFrames,
                                 const IOAudioStreamFormat *format,
                                 IOAudioStream *stream) override;

protected:
    // Hardware access used by IntelDspAudioEngine.
    bool attach(IOPCIDevice *pci);
    void detach();

    bool probe();                       // controller out of reset, in D0, capabilities parsed
    bool ready() const { return controllerReady; }
    bool ensureDspMapped();             // map BAR4 on demand
    bool memoryDecoded() const;
    bool inD0() const;
    bool waitUsable(uint32_t timeoutUs);

    uint32_t numIn() const { return numInputStreams; }
    uint32_t ppOffset() const { return ppCapOffset; }
    uint32_t sdOffset(uint32_t index) const { return 0x80 + 0x20 * index; }
    uint32_t dpibOffset(uint32_t index) const { return 0x1084 + 0x20 * index; }

    IOPCIDevice *device() const { return pciDev; }
    volatile uint8_t *hda() const { return hdaRegs; }
    volatile uint8_t *dsp() const { return dspRegs; }

    uint32_t r32(uint32_t off) const;
    uint16_t r16(uint32_t off) const;
    uint8_t  r8(uint32_t off) const;
    void     w32(uint32_t off, uint32_t value) const;
    void     w16(uint32_t off, uint16_t value) const;
    void     w8(uint32_t off, uint8_t value) const;
    uint32_t d32(uint32_t off) const;
    void     dw32(uint32_t off, uint32_t value) const;

    bool pollHda(uint32_t off, uint32_t mask, uint32_t want, uint32_t timeoutUs,
                 uint32_t *last = nullptr);
    bool pollDsp(uint32_t off, uint32_t mask, uint32_t want, uint32_t timeoutUs,
                 uint32_t *last = nullptr);

    bool walkCapabilities();
    bool streamStopReset(uint32_t sd);
    void gating(bool enable);
    void setNoSnoop(bool on);

    const NativeHdaOps *nativeOps {nullptr};

    IOPCIDevice *pciDev {nullptr};
    IOMemoryMap *bar0Map {nullptr};
    IOMemoryMap *bar4Map {nullptr};
    volatile uint8_t *hdaRegs {nullptr};
    volatile uint8_t *dspRegs {nullptr};

    uint32_t numInputStreams {0}, numOutputStreams {0}, numBidirStreams {0};
    uint32_t ppCapOffset {0}, mlCapOffset {0}, spbCapOffset {0}, drsmCapOffset {0};
    bool controllerReady {false};

    bool gatingDisabled {false};
    bool gprocenForced {false};      // PPCTL.GPROCEN set by us (firmware left it clear); cleared in gating(true)
    uint32_t savedCgctl {0}, savedPgctl {0}, savedEm2 {0};
    uint16_t savedDevc {0};
    bool devcChanged {false};
};
