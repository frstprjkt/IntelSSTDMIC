// DSP engine contract, creation request/result and dependency context.
#pragma once
#include <stdint.h>
#include "LayoutConfig.hpp"
#include "IAudioEngineHooks.hpp"
#include "NativeHdaOps.hpp"
#include "IDspHost.hpp"

class HdaAudioEngine;
class IOService;
class IOPCIDevice;

enum class DspDeviceEvent : uint32_t {
    Init = 0,
    Start,
    Stop,
    Format,
    Teardown,
    PowerLost,
    PowerRestored,
    Shutdown,
};

class IDspEventSink {
public:
    virtual ~IDspEventSink() {}
    virtual void postDeviceEvent(DspDeviceEvent event, IOService *engine) = 0;
};

struct DspEngineContext {
    const LayoutConfig &layout;
    IOPCIDevice *pci;
    IDspHost &host;
    IDspEventSink &events;
    const NativeHdaOps &ops;
    uint32_t gainPercent {100};
};

struct DspHookStats {
    uint32_t calls {0};
    uint32_t streamOurs {0};
    uint32_t engineOurs {0};
    uint32_t activeCalls {0};
    uint32_t inactive {0};
    uint32_t fillOk {0};
    uint32_t fillFail {0};
    uint32_t failMask {0};
};

class IDspAudioEngine {
public:
    virtual ~IDspAudioEngine() {}

    virtual bool initialize(const DspEngineContext &ctx) = 0;
    virtual void shutdown() = 0;

    virtual IAudioEngineHooks *hooks() = 0;

    // System sleep / PM state 0. Must be safe to call from the PM thread.
    virtual void onPowerLost() = 0;
    // System wake / PM state 1.
    virtual void onPowerRestored() = 0;

    // True when `self` is this controller's internal microphone engine.
    virtual bool ownsEngine(IOService *self) const = 0;

    virtual bool isAlive() const = 0;
    virtual bool streaming() const = 0;
    virtual bool setupDone() const = 0;
    virtual const char *lastError() const = 0;
    virtual const char *platformName() const = 0;
    virtual const char *engineName() const = 0;
    virtual PlatformFamily platform() const = 0;
    virtual const LayoutConfig &layout() const = 0;

    virtual uint64_t fills() const = 0;
    virtual uint64_t frames() const = 0;
    virtual uint32_t imicTag() const = 0;
    virtual uint32_t imicStreams() const = 0;
    virtual DspHookStats hookStats() const = 0;

    // Executed by the manager's event loop, never on the AppleHDA thread.
    virtual void handleDeviceEvent(DspDeviceEvent event, IOService *engine) = 0;

    // Shared input fill path used by the generic and specialized AppleHDA conversion hooks.
    // Returns true when the buffer was handled (filled or deliberately silenced).
    virtual bool tryFillInput(IOService *self, const void *sampleBuf, void *destBuf,
                              UInt32 firstFrame, UInt32 numFrames,
                              const IOAudioStreamFormat *format,
                              IOAudioStream *stream) = 0;
};

enum class DspEngineCreateSource : uint32_t {
    Layout = 0,
    Explicit = 1,
    AutoDetect = 2,
    Disabled = 3,
};

struct DspEngineCreateRequest {
    DspEngineCreateSource source {DspEngineCreateSource::Layout};
    const LayoutConfig *layout {nullptr};
    PlatformFamily platform {PlatformFamily::Unknown};
};

struct DspEngineCreateResult {
    IDspAudioEngine *engine {nullptr};
    IAudioEngineHooks *hooks {nullptr};
    HdaAudioEngine *native {nullptr};
    const char *error {nullptr};
    bool disabled {false};
};
