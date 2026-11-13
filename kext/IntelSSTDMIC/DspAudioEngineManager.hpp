// Manager/proxy: implements the hook interface, owns the factory result and serialises every
// device operation on a private event loop. AppleHDA native calls happen on the hook thread;
// the event loop only touches DSP/power state. PCM copying bypasses the loop entirely.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOLocks.h>
#include "IAudioEngineHooks.hpp"
#include "IDspAudioEngine.hpp"
#include "DspAudioEngineFactory.hpp"

class DspAudioEngineManager : public OSObject, public IAudioEngineHooks, public IDspEventSink {
    OSDeclareDefaultStructors(DspAudioEngineManager)
public:
    bool init(IDspAudioEngineFactory *f, IDspHost *h);
    void free() override;

    bool initialize(const DspEngineCreateRequest &request, const DspEngineContext &context);
    void shutdown();

    IAudioEngineHooks *hooks() const { return result.hooks; }
    bool ready() const { return result.hooks != nullptr; }
    bool dspActive() const { return result.engine != nullptr; }
    IDspAudioEngine *activeEngine() const { return result.engine; }
    const char *createError() const { return result.error; }
    const char *engineName() const;
    PlatformFamily platform() const;

    struct Status {
        bool dspActive {false};
        bool ready {false};
        bool disabled {false};
        const char *engineName {"native"};
        const char *createError {nullptr};
        const char *lastError {nullptr};
        bool alive {false};
        bool streaming {false};
        bool setupDone {false};
        uint64_t fills {0};
        uint64_t frames {0};
        uint32_t imicTag {0};
        uint32_t imicStreams {0};
        uint32_t layoutId {0};
        DspHookStats hooks {};
        uint32_t eventsPosted {0};
        uint32_t eventsCompleted {0};
        uint32_t eventsDropped {0};
    };
    Status status() const;

    // PM: PowerLost is synchronous so the controller is not powered down mid-teardown.
    void onPowerLostSync();
    void onPowerRestored();

    void setNativeOps(const NativeHdaOps *ops) override;

    void postDeviceEvent(DspDeviceEvent event, IOService *engine) override {
        postEvent(event, engine);
    }

    bool tryFillInput(IOService *self, const void *sampleBuf, void *destBuf,
                      UInt32 firstFrame, UInt32 numFrames,
                      const IOAudioStreamFormat *format, IOAudioStream *stream);

    // IAudioEngineHooks: native call first, DSP work follows synchronously.
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

private:
    struct Event {
        DspDeviceEvent type {DspDeviceEvent::Init};
        IOService *engine {nullptr};
        uint32_t seq {0};
    };
    static constexpr uint32_t kQueueSize = 32;

    static void timerFired(OSObject *owner, IOTimerEventSource *sender);
    // Synchronous by default; -sstdmicasync keeps the old event-loop dispatch as a fallback.
    void dispatch(DspDeviceEvent type, IOService *engine);
    uint32_t postEvent(DspDeviceEvent type, IOService *engine);
    void processEvents();
    bool waitFor(uint32_t seq, uint32_t timeoutMs);
    void drainQueue();

    IDspAudioEngineFactory *factory {nullptr};
    IDspHost *host {nullptr};
    DspEngineCreateResult result {};
    const NativeHdaOps *ops {nullptr};

    IOWorkLoop *loop {nullptr};
    IOTimerEventSource *timer {nullptr};
    IOSimpleLock *queueLock {nullptr};

    Event queue[kQueueSize] {};
    uint32_t head {0}, tail {0}, count {0};
    volatile uint32_t postedSeq {0}, completedSeq {0}, droppedEvents {0};
    volatile bool accepting {false};
    volatile bool processing {false};
    bool asyncMode {false};
    bool shutDown {false};
};
