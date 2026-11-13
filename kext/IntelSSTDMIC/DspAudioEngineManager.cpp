#include "DspAudioEngineManager.hpp"
#include "BootArgs.hpp"
#include <IOKit/IOLib.h>

#define super OSObject
OSDefineMetaClassAndStructors(DspAudioEngineManager, OSObject)

bool DspAudioEngineManager::init(IDspAudioEngineFactory *f, IDspHost *h) {
    if (!super::init()) return false;
    factory = f;
    host = h;
    asyncMode = BootArgs::present("sstdmicasync");
    if (asyncMode && host) host->logLine("DSP events on event loop (-sstdmicasync)");

    queueLock = IOSimpleLockAlloc();
    loop = IOWorkLoop::workLoop();
    if (!queueLock || !loop) {
        OSSafeReleaseNULL(loop);
        if (queueLock) { IOSimpleLockFree(queueLock); queueLock = nullptr; }
        return false;
    }

    timer = IOTimerEventSource::timerEventSource(this, timerFired);
    if (!timer || loop->addEventSource(timer) != kIOReturnSuccess) {
        OSSafeReleaseNULL(timer);
        OSSafeReleaseNULL(loop);
        IOSimpleLockFree(queueLock);
        queueLock = nullptr;
        return false;
    }

    accepting = true;
    return true;
}

void DspAudioEngineManager::free() {
    shutdown();
    if (timer) {
        if (loop && !shutDown) loop->removeEventSource(timer);
        OSSafeReleaseNULL(timer);
    }
    OSSafeReleaseNULL(loop);
    if (queueLock) {
        IOSimpleLockFree(queueLock);
        queueLock = nullptr;
    }
    super::free();
}

bool DspAudioEngineManager::initialize(const DspEngineCreateRequest &request,
                                       const DspEngineContext &context) {
    if (result.hooks) return true;
    host = &context.host;
    ops = &context.ops;
    result = factory->create(request, context);
    if (host) {
        if (result.disabled)
            host->logLine("layout disabled (0) - using native AppleHDA path");
        else if (!result.engine)
            host->logLine("DSP engine creation failed: %s - using native AppleHDA path",
                          result.error ? result.error : "unknown");
    }
    accepting = true;
    return result.hooks != nullptr;
}

void DspAudioEngineManager::shutdown() {
    if (shutDown) return;
    shutDown = true;
    accepting = false;

    if (loop) {
        bool pending = true;
        for (uint32_t i = 0; i < 5000 && pending; i++) {
            IOSimpleLockLock(queueLock);
            pending = count != 0 || processing;
            IOSimpleLockUnlock(queueLock);
            if (pending) IOSleep(1);
        }
        if (pending && host) host->logLine("event loop did not drain before shutdown");
        if (timer) {
            timer->cancelTimeout();
            loop->removeEventSource(timer);
        }
    }

    drainQueue();
    if (factory) factory->destroy(result);
    result = DspEngineCreateResult {};
    ops = nullptr;
}

void DspAudioEngineManager::drainQueue() {
    if (!queueLock) return;
    IOSimpleLockLock(queueLock);
    while (count) {
        Event &e = queue[tail];
        IOService *engine = e.engine;
        e.engine = nullptr;
        tail = (tail + 1) % kQueueSize;
        count--;
        IOSimpleLockUnlock(queueLock);
        if (engine) engine->release();
        IOSimpleLockLock(queueLock);
    }
    IOSimpleLockUnlock(queueLock);
}

void DspAudioEngineManager::dispatch(DspDeviceEvent type, IOService *engine) {
    if (!result.engine) return;
    if (asyncMode) postEvent(type, engine);
    else result.engine->handleDeviceEvent(type, engine);
}

uint32_t DspAudioEngineManager::postEvent(DspDeviceEvent type, IOService *engine) {
    if (!result.engine || !queueLock || !accepting) return 0;
    if (engine) engine->retain();
    uint32_t seq = 0;
    IOSimpleLockLock(queueLock);
    if (!accepting || count == kQueueSize) {
        IOSimpleLockUnlock(queueLock);
        if (engine) engine->release();
        __sync_add_and_fetch(&droppedEvents, 1);
        return 0;
    }
    Event &e = queue[head];
    e.type = type;
    e.engine = engine;
    seq = __sync_add_and_fetch(&postedSeq, 1);
    e.seq = seq;
    head = (head + 1) % kQueueSize;
    count++;
    IOSimpleLockUnlock(queueLock);
    if (timer) timer->setTimeoutMS(1);
    return seq;
}

void DspAudioEngineManager::timerFired(OSObject *owner, IOTimerEventSource *) {
    auto self = OSDynamicCast(DspAudioEngineManager, owner);
    if (self) self->processEvents();
}

void DspAudioEngineManager::processEvents() {
    if (processing) return;
    processing = true;
    for (;;) {
        Event e {};
        IOSimpleLockLock(queueLock);
        if (!count) {
            IOSimpleLockUnlock(queueLock);
            break;
        }
        e = queue[tail];
        queue[tail].engine = nullptr;
        tail = (tail + 1) % kQueueSize;
        count--;
        IOSimpleLockUnlock(queueLock);

        if (result.engine) result.engine->handleDeviceEvent(e.type, e.engine);
        if (e.engine) e.engine->release();
        __sync_add_and_fetch(&completedSeq, 1);
    }
    processing = false;

    bool pending = false;
    IOSimpleLockLock(queueLock);
    pending = count != 0;
    IOSimpleLockUnlock(queueLock);
    if (pending && timer) timer->setTimeoutMS(1);
}

bool DspAudioEngineManager::waitFor(uint32_t seq, uint32_t timeoutMs) {
    if (!seq) return true;
    for (uint32_t t = 0; t < timeoutMs; t++) {
        if ((uint32_t)completedSeq >= seq) return true;
        IOSleep(1);
    }
    return (uint32_t)completedSeq >= seq;
}

void DspAudioEngineManager::onPowerLostSync() {
    if (result.engine) result.engine->onPowerLost();
}

void DspAudioEngineManager::onPowerRestored() {
    if (result.engine) result.engine->onPowerRestored();
}

void DspAudioEngineManager::setNativeOps(const NativeHdaOps *newOps) {
    ops = newOps;
    if (result.hooks) result.hooks->setNativeOps(newOps);
}

const char *DspAudioEngineManager::engineName() const {
    return result.engine ? result.engine->engineName() : "native";
}

PlatformFamily DspAudioEngineManager::platform() const {
    return result.engine ? result.engine->platform() : PlatformFamily::Unknown;
}

DspAudioEngineManager::Status DspAudioEngineManager::status() const {
    Status s {};
    s.ready = result.hooks != nullptr;
    s.dspActive = result.engine != nullptr;
    s.disabled = result.disabled;
    s.engineName = engineName();
    s.createError = result.error;
    s.eventsPosted = postedSeq;
    s.eventsCompleted = completedSeq;
    s.eventsDropped = droppedEvents;
    if (result.engine) {
        s.alive = result.engine->isAlive();
        s.streaming = result.engine->streaming();
        s.setupDone = result.engine->setupDone();
        s.lastError = result.engine->lastError();
        s.fills = result.engine->fills();
        s.frames = result.engine->frames();
        s.imicTag = result.engine->imicTag();
        s.imicStreams = result.engine->imicStreams();
        s.layoutId = result.engine->layout().id;
        s.hooks = result.engine->hookStats();
    } else {
        s.lastError = result.error;
    }
    return s;
}

bool DspAudioEngineManager::tryFillInput(IOService *self, const void *sampleBuf, void *destBuf,
                                         UInt32 firstFrame, UInt32 numFrames,
                                         const IOAudioStreamFormat *format, IOAudioStream *stream) {
    return result.engine
        ? result.engine->tryFillInput(self, sampleBuf, destBuf, firstFrame, numFrames, format, stream)
        : false;
}

// ---- hook dispatch ------------------------------------------------------------------------

bool DspAudioEngineManager::initHardware(IOService *self, IOService *provider) {
    bool r = result.hooks ? result.hooks->initHardware(self, provider)
                          : (ops && ops->initHardware ? ops->initHardware(self, provider) : false);
    if (host) host->logLine("hook initHardware (native %d)", (int)r);
    if (r) dispatch(DspDeviceEvent::Init, self);
    return r;
}

IOReturn DspAudioEngineManager::performAudioEngineStart(IOService *self) {
    IOReturn r = result.hooks ? result.hooks->performAudioEngineStart(self)
                              : (ops && ops->performAudioEngineStart ? ops->performAudioEngineStart(self) : kIOReturnUnsupported);
    if (host) host->logLine("hook performAudioEngineStart (native 0x%x)", (unsigned)r);
    dispatch(DspDeviceEvent::Start, self);
    return r;
}

IOReturn DspAudioEngineManager::performAudioEngineStop(IOService *self) {
    IOReturn r = result.hooks ? result.hooks->performAudioEngineStop(self)
                              : (ops && ops->performAudioEngineStop ? ops->performAudioEngineStop(self) : kIOReturnUnsupported);
    if (host) host->logLine("hook performAudioEngineStop (native 0x%x)", (unsigned)r);
    dispatch(DspDeviceEvent::Stop, self);
    return r;
}

void DspAudioEngineManager::stopHardware(IOService *self, IOService *provider) {
    if (result.hooks) result.hooks->stopHardware(self, provider);
    else if (ops && ops->stopHardware) ops->stopHardware(self, provider);
    if (host) host->logLine("hook stopHardware");
    dispatch(DspDeviceEvent::Teardown, self);
}

IOReturn DspAudioEngineManager::performFormatChange(IOService *self, IOAudioStream *stream,
                                                    const IOAudioStreamFormat *format,
                                                    const IOAudioSampleRate *rate) {
    IOReturn r = result.hooks ? result.hooks->performFormatChange(self, stream, format, rate)
                              : (ops && ops->performFormatChange ? ops->performFormatChange(self, stream, format, rate) : kIOReturnUnsupported);
    dispatch(DspDeviceEvent::Format, self);
    return r;
}

IOReturn DspAudioEngineManager::convertInputSamples(IOService *self, const void *sampleBuf,
                                                    void *destBuf, UInt32 firstFrame,
                                                    UInt32 numFrames,
                                                    const IOAudioStreamFormat *format,
                                                    IOAudioStream *stream) {
    if (result.hooks)
        return result.hooks->convertInputSamples(self, sampleBuf, destBuf, firstFrame, numFrames, format, stream);
    return ops && ops->convertInputSamples
        ? ops->convertInputSamples(self, sampleBuf, destBuf, firstFrame, numFrames, format, stream)
        : kIOReturnUnsupported;
}
