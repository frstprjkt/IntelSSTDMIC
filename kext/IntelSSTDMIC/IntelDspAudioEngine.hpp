// Common DSP engine: firmware loader, IPC3, host ring, session/power state and the sample path.
//
// Hook methods are synchronous. Each override calls the native HdaAudioEngine first (or last, for
// stop) and then performs the DSP work for this controller's internal microphone only. The sample
// path is lock-free: `active` gates fills, `inFlight` drains them before teardown.
#pragma once
#include "HdaAudioEngine.hpp"
#include "IDspAudioEngine.hpp"
#include "DmaBuffer.hpp"

class OSArray;
struct IpcMsg;

class IntelDspAudioEngine : public HdaAudioEngine, public IDspAudioEngine {
public:
    IntelDspAudioEngine() = default;
    ~IntelDspAudioEngine() override;

    bool initialize(const DspEngineContext &ctx) override;
    void shutdown() override;
    IAudioEngineHooks *hooks() override { return this; }
    void onPowerLost() override;
    void onPowerRestored() override;
    bool ownsEngine(IOService *self) const override;

    bool isAlive() const override;
    bool streaming() const override { return active; }
    bool setupDone() const override { return pipelineReady; }
    const char *lastError() const override { return errorText; }
    const char *platformName() const override = 0;
    const char *engineName() const override = 0;
    PlatformFamily platform() const override = 0;
    const LayoutConfig &layout() const override { return *cfg; }
    uint64_t fills() const override { return fillCount; }
    uint64_t frames() const override { return frameCount; }
    uint32_t imicTag() const override { return imicTagValue; }
    uint32_t imicStreams() const override { return imicStreamCount; }
    DspHookStats hookStats() const override;
    bool tryFillInput(IOService *self, const void *sampleBuf, void *destBuf,
                      UInt32 firstFrame, UInt32 numFrames,
                      const IOAudioStreamFormat *format,
                      IOAudioStream *stream) override;

    void handleDeviceEvent(DspDeviceEvent event, IOService *engine) override;

    // IAudioEngineHooks: lifecycle hooks stay native; the Manager posts device events.
    IOReturn convertInputSamples(IOService *self, const void *sampleBuf, void *destBuf,
                                 UInt32 firstFrame, UInt32 numFrames,
                                 const IOAudioStreamFormat *format,
                                 IOAudioStream *stream) override;

protected:
    // Platform differences.
    virtual const uint8_t *firmwareImage(uint32_t &size) const = 0;
    virtual uint32_t targetCoreMask() const = 0;
    virtual uint32_t defaultCaptureIndex() const { return 6; }
    virtual uint32_t defaultCaptureTag() const { return 7; }

    void log(const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    bool fail(const char *what, uint32_t value = 0);

    // Engine identity / descriptor.
    bool isInternalMicEngine(IOService *self) const;
    bool noteEngine(IOService *self);
    void releaseEngineCache();
    bool findDescriptorByTag(uint32_t tag, uint32_t &descriptorIndex,
                             uint16_t &fmt, uint32_t &cbl) const;
    bool findRunningDescriptorByTag(uint32_t tag, uint32_t &descriptorIndex,
                                    uint16_t &fmt, uint32_t &cbl) const;
    void syncGeometry(uint16_t fmt, uint32_t cbl);
    void refreshGeometry();

    // Session (lock-free active/inFlight).
    bool initializeDsp();
    bool startSession(IOService *self);
    void stopStreaming(const char *why);
    void powerDownSession(const char *why);
    void rollbackSession();

    // DSP boot / pipeline.
    bool bootFirmware();
    bool startStream();
    void stopStream();
    bool ensureBuffers();
    void releaseBuffers();
    bool prepareIpcStream(uint32_t idx, uint32_t tag);
    bool loaderStream(uint32_t idx, uint32_t tag, size_t bytes);
    void releaseLoaderStream(uint32_t idx);
    bool corePowerUp();
    bool coreRun();
    void corePowerDownInternal();
    bool dspAlive(bool logReason);
    bool ipcSend(const uint32_t *msg, uint32_t words, uint32_t *reply = nullptr,
                 uint32_t replyWords = 0);
    void ackDspNotification();
    bool replay(const IpcMsg *list, uint32_t count, const char *phase);

    // Sample path.
    bool fillFromRing(void *destBuf, uint32_t firstFrame, uint32_t numFrames,
                      const IOAudioStreamFormat *format);
    bool isOurStream(void *audioStream) const;

    static constexpr uint32_t kRingBytes = 0x10000;
    static constexpr uint32_t kPeriodBytes = 0x4000;

    const LayoutConfig *cfg {nullptr};
    IDspHost *host {nullptr};
    IDspEventSink *events {nullptr};
    uint32_t gainQ16 {1u << 16};
    uint32_t chanMap[8] {0, 1, 2, 3};

    uint32_t captureIdx {6}, captureTag {7};
    uint32_t srcChannels {4}, srcFrameBytes {16};
    uint32_t imicTagValue {0}, imicSafetyFrames {57};

    IOService *imicEngine {nullptr};
    IOService *imicStreamList[8] {};
    uint32_t imicStreamCount {0};

    uint32_t engineDesc {0}, engineFrameBytes {0}, engineRingFrames {0}, engineBits {32};
    uint64_t engineStep {1ULL << 32};
    uint16_t lastFmt {0};
    uint32_t lastCbl {0};

    volatile bool active {false};
    volatile uint32_t inFlight {0};
    volatile uint32_t sessionState {0};   // 0 idle, 1 starting, 2 streaming, 3 stopping
    volatile bool powerLost {false};

    bool bootDone {false};
    bool pipelineReady {false};
    bool triggered {false};
    char errorText[128] {};

    DmaBuffer clBuf, bdlBuf, ringBuf, ptBuf, capBdl;

    bool dspInit {false};
    uint64_t dspQ {0}, dspTime {0};
    uint64_t fillCount {0}, frameCount {0};
    volatile uint32_t hookCalls {0}, hookStreamOurs {0}, hookEngineOurs {0};
    volatile uint32_t hookActive {0}, hookInactive {0}, hookFillOk {0}, hookFillFail {0};
    volatile uint32_t fillFailMask {0};
    uint32_t loggedFailMask {0};
};
