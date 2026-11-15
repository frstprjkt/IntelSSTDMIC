#include "IntelDspAudioEngine.hpp"
#include "Regs.hpp"
#include "IpcReplay.hpp"
#include "IpcReplay2ch.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/clock.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSNumber.h>
#include <stdarg.h>

#define LOG(fmt, ...) log(fmt, ##__VA_ARGS__)
#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

namespace {
constexpr uint32_t SD_CTL_INT_MASK = 0x1C;
constexpr uint32_t SD_TAG_MASK = 0xFu << 20;
constexpr uint16_t CL_FORMAT = 0x40;
constexpr uint32_t ROM_IPC_CONTROL = 0x01000000, ROM_IPC_PURGE_FW = 0x00004000;
constexpr uint32_t FSR_MASK = 0x00FFFFFF, FSR_INIT_DONE = 0x1, FSR_FW_ENTERED = 0x5;
constexpr uint32_t CORE0 = 1;
constexpr uint32_t XMAN_MAGIC = 0x6e614d58;
constexpr uint32_t VS_D0I3C = 0x104A, D0I3C_I3 = 1u << 2;
constexpr uint32_t SPB_SPBFCCTL = 0x04, SPB_SPIB_BASE = 0x08, SPB_SPIB_STRIDE = 0x08;
constexpr uint32_t PAGES = 0x10000 / 4096;
// HDA SD_FMT: BASE 48 kHz, BITS = 100 (32-bit container), CHAN = channels - 1
constexpr uint16_t FMT_48K_32BIT = 0x0040;
// Captured SOF pipelines (linux/extract-ipc.py). Same topology (pipeline 10); the 2 ch one differs in
// DAI_CONFIG (1 PDM controller), buffer sizes, the volume component's channel count and the PCM channels.
const IpcPipeline kPipe4ch = { kIpc_setup, COUNT(kIpc_setup), kIpc_start, COUNT(kIpc_start), kIpc_stop,
    COUNT(kIpc_stop), kIpc_teardown, COUNT(kIpc_teardown), 4, "HP EliteBook 840 G8" };
const IpcPipeline kPipe2ch = { kIpc2ch_setup, COUNT(kIpc2ch_setup), kIpc2ch_start, COUNT(kIpc2ch_start),
    kIpc2ch_stop, COUNT(kIpc2ch_stop), kIpc2ch_teardown, COUNT(kIpc2ch_teardown), 2,
    "Lenovo XiaoXin Pro 13 2020" };

inline uint32_t SPA(uint32_t m) { return m << 16; }
inline uint32_t CPA(uint32_t m) { return m << 24; }
inline uint32_t CRST(uint32_t m) { return m; }
inline uint32_t CSTALL(uint32_t m) { return m << 8; }

const uint32_t kBitDepth[8] = {8, 16, 20, 24, 32, 0, 0, 0};

uint32_t fmtRate(uint16_t fmt) {
    uint32_t base = (fmt & 0x4000) ? 44100 : 48000;
    uint32_t mult = ((fmt >> 11) & 0x7) + 1;
    uint32_t div = ((fmt >> 8) & 0x7) + 1;
    return base * mult / div;
}
}

IntelDspAudioEngine::~IntelDspAudioEngine() {
    releaseBuffers();
    releaseEngineCache();
    detach();
}

void IntelDspAudioEngine::log(const char *fmt, ...) {
    char buf[200];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    IOLog("IntelSSTDMIC: %s\n", buf);
    if (host) host->logLine("%s", buf);
}

bool IntelDspAudioEngine::fail(const char *what, uint32_t value) {
    snprintf(errorText, sizeof(errorText), "FAIL %s (0x%08x)", what, value);
    log("%s", errorText);
    return false;
}

// ---- initialization -----------------------------------------------------------------------

bool IntelDspAudioEngine::initialize(const DspEngineContext &ctx) {
    cfg = &ctx.layout;
    host = &ctx.host;
    events = &ctx.events;
    setNativeOps(&ctx.ops);

    if (!cfg->valid) return false;
    if (cfg->sourceBits != 32) {
        snprintf(errorText, sizeof(errorText), "layout %u: DSP ring must be S32_LE", cfg->id);
        return false;
    }
    srcChannels = cfg->sourceChannels ? cfg->sourceChannels : 4;
    if (srcChannels != 2 && srcChannels != 4) {
        snprintf(errorText, sizeof(errorText), "layout %u: sourceChannels must be 2 or 4 (S32_LE)", cfg->id);
        return false;
    }
    ipc = (srcChannels == 2) ? &kPipe2ch : &kPipe4ch;
    srcFrameBytes = srcChannels * 4;

    captureIdx = cfg->streamIndex ? cfg->streamIndex : defaultCaptureIndex();
    captureTag = cfg->pcmTag ? cfg->pcmTag : defaultCaptureTag();
    gainQ16 = (uint32_t)((uint64_t)(ctx.gainPercent ? ctx.gainPercent : cfg->gainPercent) * 65536 / 100);
    for (uint32_t i = 0; i < 8; i++) chanMap[i] = cfg->channelMap[i] & 3;

    if (!attach(ctx.pci)) {
        snprintf(errorText, sizeof(errorText), "BAR0 mapping failed");
        return false;
    }
    log("layout %u (%s): %s, %u mic, source %uch/%uHz, engine %s",
        cfg->id, cfg->name, platformName(), cfg->micCount, srcChannels, cfg->sourceRate, engineName());
    return true;
}

void IntelDspAudioEngine::shutdown() {
    // A start hook or firmware boot may be in progress on another thread; let it finish first.
    for (uint32_t i = 0; i < 3000 && (sessionState == 1 || dspBootState == 1); i++) IOSleep(1);
    powerDownSession("shutdown");
    releaseBuffers();
    releaseEngineCache();
    host = nullptr;
    cfg = nullptr;
    detach();
}

bool IntelDspAudioEngine::ownsEngine(IOService *self) const {
    if (!self) return false;
    if (imicEngine == self) return true;
    IORegistryEntry *e = self;
    for (int depth = 0; e && depth < 8; depth++) {
        if (e == static_cast<IORegistryEntry *>(device())) return isInternalMicEngine(self);
        e = e->getParentEntry(gIOServicePlane);
    }
    return false;
}

bool IntelDspAudioEngine::isInternalMicEngine(IOService *self) const {
    if (!self) return false;
    OSString *desc = OSDynamicCast(OSString, self->getProperty("IOAudioEngineDescription"));
    return desc && desc->isEqualTo("DevShortNameIMic");
}

bool IntelDspAudioEngine::isAlive() const {
    if (!bootDone || !memoryDecoded() || !inD0() || !dsp()) return false;
    uint32_t cs = d32(DSP::ADSPCS);
    if (cs == 0xFFFFFFFF) return false;
    if (!(cs & CPA(CORE0))) return false;
    if (cs & CSTALL(CORE0)) return false;
    if (cs & CRST(CORE0)) return false;
    return true;
}

// ---- engine identity ----------------------------------------------------------------------

bool IntelDspAudioEngine::noteEngine(IOService *self) {
    if (!self || !ownsEngine(self)) return false;
    if (!isInternalMicEngine(self)) return false;

    OSString *uid = OSDynamicCast(OSString, self->getProperty("IOAudioEngineGlobalUniqueID"));
    const char *s = uid ? uid->getCStringNoCopy() : nullptr;
    const char *colon = nullptr;
    for (const char *p = s; p && *p; p++)
        if (*p == ':') colon = p;
    if (!colon) return false;
    uint32_t tag = 0;
    for (const char *p = colon + 1; *p >= '0' && *p <= '9'; p++)
        tag = tag * 10 + (uint32_t)(*p - '0');
    if (!tag || tag > 15) return false;
    uint32_t safety = imicSafetyFrames;
    if (OSNumber *off = OSDynamicCast(OSNumber, self->getProperty("IOAudioEngineInputSampleOffset")))
        if (off->unsigned32BitValue() > 0) safety = off->unsigned32BitValue();

    bool changed = (self != imicEngine) || (tag != imicTagValue) || (safety != imicSafetyFrames);
    imicTagValue = tag;
    imicSafetyFrames = safety;

    if (self != imicEngine || imicStreamCount == 0) {
        self->retain();
        releaseEngineCache();      // releases the previous engine
        imicEngine = self;
        imicTagValue = tag;
        imicSafetyFrames = safety;
        OSIterator *kids = self->getChildIterator(gIOServicePlane);
        if (kids) {
            while (OSObject *o = kids->getNextObject()) {
                IOService *svc = OSDynamicCast(IOService, o);
                if (!svc) continue;
                const OSMetaClass *mc = svc->getMetaClass();
                const char *cn = mc ? mc->getClassName() : nullptr;
                if (!cn || strcmp(cn, "AppleHDAStream") != 0) continue;
                if (imicStreamCount >= COUNT(imicStreamList)) break;
                svc->retain();
                imicStreamList[imicStreamCount++] = svc;
            }
            kids->release();
        }
    }
    if (changed)
        log("internal mic engine %s -> tag %u, safety %u, %u stream(s)",
            s ? s : "?", imicTagValue, imicSafetyFrames, imicStreamCount);
    return true;
}

void IntelDspAudioEngine::releaseEngineCache() {
    for (uint32_t i = 0; i < imicStreamCount; i++) OSSafeReleaseNULL(imicStreamList[i]);
    imicStreamCount = 0;
    OSSafeReleaseNULL(imicEngine);
    imicTagValue = 0;
    engineDesc = 0;
}

bool IntelDspAudioEngine::isOurStream(void *audioStream) const {
    for (uint32_t i = 0; i < imicStreamCount; i++)
        if (static_cast<void *>(imicStreamList[i]) == audioStream) return true;
    return false;
}

bool IntelDspAudioEngine::findDescriptorByTag(uint32_t tag, uint32_t &descriptorIndex,
                                          uint16_t &fmt, uint32_t &cbl) const {
    for (uint32_t i = 0; i < numIn(); i++) {
        if (i == captureIdx) continue;
        uint32_t sd = sdOffset(i);
        uint32_t ctl = r32(sd);
        if (ctl == 0xFFFFFFFF) continue;
        if (r32(ppOffset() + HDA::PP_PPCTL) & (1u << i)) continue;
        uint16_t f = *reinterpret_cast<volatile uint16_t *>(hda() + sd + HDA::SD_FMT);
        uint32_t c = r32(sd + HDA::SD_CBL);
        if (!f || !c) continue;
        if (((ctl >> HDA::SD_CTL_TAG_SHIFT) & 0xF) != tag) continue;
        descriptorIndex = i; fmt = f; cbl = c;
        return true;
    }
    return false;
}

bool IntelDspAudioEngine::findRunningDescriptorByTag(uint32_t tag, uint32_t &descriptorIndex,
                                                  uint16_t &fmt, uint32_t &cbl) const {
    for (uint32_t i = 0; i < numIn(); i++) {
        if (i == captureIdx) continue;
        uint32_t sd = sdOffset(i);
        uint32_t ctl = r32(sd);
        if (ctl == 0xFFFFFFFF || !(ctl & HDA::SD_CTL_RUN)) continue;
        if (r32(ppOffset() + HDA::PP_PPCTL) & (1u << i)) continue;
        uint16_t f = *reinterpret_cast<volatile uint16_t *>(hda() + sd + HDA::SD_FMT);
        uint32_t c = r32(sd + HDA::SD_CBL);
        if (!f || !c) continue;
        if (((ctl >> HDA::SD_CTL_TAG_SHIFT) & 0xF) != tag) continue;
        descriptorIndex = i; fmt = f; cbl = c;
        return true;
    }
    return false;
}

DspHookStats IntelDspAudioEngine::hookStats() const {
    DspHookStats s {};
    s.calls = hookCalls; s.streamOurs = hookStreamOurs; s.engineOurs = hookEngineOurs;
    s.activeCalls = hookActive; s.inactive = hookInactive;
    s.fillOk = hookFillOk; s.fillFail = hookFillFail;
    s.failMask = fillFailMask;
    return s;
}

void IntelDspAudioEngine::syncGeometry(uint16_t fmt, uint32_t cbl) {
    uint32_t ch = (fmt & 0xF) + 1;
    uint32_t bits = kBitDepth[(fmt >> 4) & 0x7];
    uint32_t rate = fmtRate(fmt);
    if (!rate) rate = 48000;
    uint32_t fb = (ch && ch <= 8 && bits) ? ch * (bits == 16 ? 2 : 4) : 0;
    uint32_t rf = fb ? cbl / fb : 0;
    if (!fb || !rf) return;
    engineFrameBytes = fb;
    engineRingFrames = rf;
    engineBits = bits;
    engineStep = ((uint64_t)48000 << 32) / rate;
    lastFmt = fmt;
    lastCbl = cbl;
    dspInit = false;
}

void IntelDspAudioEngine::refreshGeometry() {
    if (!active || !engineDesc) return;
    uint32_t sd = sdOffset(engineDesc);
    uint16_t fmt = *reinterpret_cast<volatile uint16_t *>(hda() + sd + HDA::SD_FMT);
    uint32_t cbl = r32(sd + HDA::SD_CBL);
    if (fmt && cbl) syncGeometry(fmt, cbl);
}

// ---- hook overrides -----------------------------------------------------------------------

void IntelDspAudioEngine::handleDeviceEvent(DspDeviceEvent event, IOService *engine) {
    switch (event) {
        case DspDeviceEvent::Init:
            if (!active && noteEngine(engine)) initializeDsp();
            break;
        case DspDeviceEvent::Start:
            if (noteEngine(engine)) startSession(engine);
            break;
        case DspDeviceEvent::Stop:
            if (ownsEngine(engine)) stopStreaming("engine stop");
            break;
        case DspDeviceEvent::Format:
            if (ownsEngine(engine)) refreshGeometry();
            break;
        case DspDeviceEvent::Teardown:
            if (ownsEngine(engine)) stopStreaming("engine teardown");
            powerDownSession("engine teardown");
            releaseEngineCache();
            break;
        case DspDeviceEvent::PowerLost:
            onPowerLost();
            break;
        case DspDeviceEvent::PowerRestored:
            onPowerRestored();
            break;
        case DspDeviceEvent::Shutdown:
            powerDownSession("shutdown");
            break;
    }
}

IOReturn IntelDspAudioEngine::convertInputSamples(IOService *self, const void *sampleBuf, void *destBuf,
                                             UInt32 firstFrame, UInt32 numFrames,
                                             const IOAudioStreamFormat *format,
                                             IOAudioStream *stream) {
    if (tryFillInput(self, sampleBuf, destBuf, firstFrame, numFrames, format, stream))
        return kIOReturnSuccess;
    return HdaAudioEngine::convertInputSamples(self, sampleBuf, destBuf, firstFrame,
                                               numFrames, format, stream);
}

bool IntelDspAudioEngine::tryFillInput(IOService *self, const void *, void *destBuf,
                                  UInt32 firstFrame, UInt32 numFrames,
                                  const IOAudioStreamFormat *format,
                                  IOAudioStream *stream) {
    if (!destBuf || !numFrames) return false;
    hookCalls++;

    // The first hook argument is AppleHDA's IOAudioEngine. Its native state is authoritative for
    // whether the engine is actually running; our own session flag may be behind PM transitions.
    // Read the native engine state from AppleHDA's own object (the hook's first argument).
    // IOAudioEngine publishes IOAudioEngineState (0 stopped, 1 running, 2 paused, 3 resumed).
    bool nativeRunning = false;
    if (self) {
        if (OSNumber *state = OSDynamicCast(OSNumber, self->getProperty("IOAudioEngineState")))
            nativeRunning = state->unsigned32BitValue() != 0;
    }

    __sync_add_and_fetch(&inFlight, 1);
    bool streamOurs = isOurStream(stream);
    bool engineOurs = ownsEngine(self);
    if (streamOurs) hookStreamOurs++;
    if (engineOurs) hookEngineOurs++;
    bool ours = streamOurs || engineOurs;

    // Recording is started only by the performAudioEngineStart hook; this data path never
    // triggers session setup. If it is not active yet, keep the native buffer silent below.

    bool handled = false;
    if (active) hookActive++;
    if (active && ours) {
        if (fillFromRing(destBuf, firstFrame, numFrames, format)) {
            fillCount++;
            frameCount += numFrames;
            hookFillOk++;
        } else {
            hookFillFail++;
            uint32_t ch = format ? format->fNumChannels : 2;
            if (!ch || ch > 8) ch = 2;
            bool toFloat = format ? (format->fIsMixable != 0) : true;
            uint32_t dstFrame = toFloat ? 4 : (engineBits == 16 ? 2 : 4);
            bzero(destBuf, (size_t)numFrames * ch * dstFrame);

            // Never fail silently: the first time a new failure reason appears, dump the stream
            // state so a bad index / dead DPIB is visible immediately.
            uint32_t newBits = fillFailMask & ~loggedFailMask;
            if (newBits) {
                loggedFailMask |= newBits;
                uint32_t sd = sdOffset(engineDesc);
                log("fill failed mask=0x%x desc=%u idx=%u tag=%u ctl=0x%x lpib=%u dpib=0x%x",
                    fillFailMask, engineDesc, captureIdx, imicTagValue,
                    r32(sd), r32(sd + HDA::SD_LPIB), r32(dpibOffset(captureIdx)));
            }
        }
        handled = true;
    } else if (!active) {
        hookInactive++;
        if (nativeRunning && ours) {
            uint32_t ch = format ? format->fNumChannels : 2;
            if (!ch || ch > 8) ch = 2;
            bool toFloat = format ? (format->fIsMixable != 0) : true;
            uint32_t dstFrame = toFloat ? 4 : (engineBits == 16 ? 2 : 4);
            bzero(destBuf, (size_t)numFrames * ch * dstFrame);
            handled = true;
        }
    }
    __sync_sub_and_fetch(&inFlight, 1);
    return handled;
}

// ---- session ------------------------------------------------------------------------------

void IntelDspAudioEngine::onPowerLost() {
    powerLost = true;
    powerDownSession("power lost");
}

bool IntelDspAudioEngine::startSession(IOService *self) {
    if (active) return true;
    if (!__sync_bool_compare_and_swap(&sessionState, 0u, 1u)) return false;

    if (!noteEngine(self) || !host || !host->holdPower()) {
        fail("engine/power", imicTagValue);
        rollbackSession();
        return false;
    }
    if (!probe() || !ensureDspMapped()) {
        fail("controller not ready");
        rollbackSession();
        return false;
    }

    // The layout's DSP stream index can exceed the controller's implemented input descriptors
    // (e.g. 6 on a controller advertising fewer inputs). Reading DPIB for such an index returns
    // 0xFFFFFFFF, which used to make every fill fail. Use the last implemented input, like the
    // original driver: min(6, numIn - 1), tag = index + 1.
    if (numInputStreams && captureIdx >= numInputStreams) {
        uint32_t clamped = numInputStreams - 1;
        log("capture stream %u out of range (%u input stream(s)) - using %u, tag %u",
            captureIdx, numInputStreams, clamped, clamped + 1);
        captureIdx = clamped;
        captureTag = clamped + 1;
    }

    uint32_t desc = 0, cbl = 0; uint16_t fmt = 0;
    if (!findDescriptorByTag(imicTagValue, desc, fmt, cbl)) {
        fail("engine descriptor (tag)", imicTagValue);
        rollbackSession();
        return false;
    }
    engineDesc = desc;
    syncGeometry(fmt, cbl);
    if (!engineFrameBytes || !engineRingFrames) {
        fail("invalid engine format", fmt);
        rollbackSession();
        return false;
    }
    if (powerLost) {
        rollbackSession();
        return false;
    }
    if (!isAlive() && !initializeDsp()) {
        rollbackSession();
        return false;
    }
    if (!startStream()) {
        rollbackSession();
        return false;
    }

    // Early self-check: a capture index the controller does not implement shows up as an
    // all-ones DPIB long before the first fill. Make that visible instead of streaming silence.
    if (r32(dpibOffset(captureIdx)) == 0xFFFFFFFF)
        log("WARNING: DPIB unreadable for capture stream %u - no audio will be produced",
            captureIdx);
    loggedFailMask = 0;

    // Commit: active first, then publish the streaming state. A concurrent PM stop either sees
    // state 1 (sets powerLost, which is checked below) or state 2 (stops the session itself).
    active = true;
    __sync_synchronize();
    sessionState = 2;
    if (powerLost) {
        powerDownSession("power lost during start");
        return false;
    }
    log("streaming into descriptor %u (tag %u), %u B/frame, %u frames",
        engineDesc, imicTagValue, engineFrameBytes, engineRingFrames);
    if (host) host->stateChanged();
    return true;
}

void IntelDspAudioEngine::onPowerRestored() {
    powerLost = false;
    // HPSRAM is gone after a real system sleep. Do not boot or restart here: this runs on the PM
    // thread and may race the first recording. The next performAudioEngineStart reinitialises the
    // DSP from a known state.
    dspBootState = isAlive() ? 2u : 0u;
    log("power restored - DSP will be reinitialised on next start");
}

void IntelDspAudioEngine::rollbackSession() {
    active = false;
    __sync_synchronize();
    for (uint32_t i = 0; i < 20 && inFlight; i++) IOSleep(1);
    if (pipelineReady || triggered) stopStream();
    if (bootDone) {
        corePowerDownInternal();
        gating(true);
        bootDone = false;
    }
    if (host) host->releasePower();
    pipelineReady = false;
    triggered = false;
    sessionState = 0;
    dspBootState = 0;
    dspInit = false;
    if (host) host->stateChanged();
}

void IntelDspAudioEngine::stopStreaming(const char *why) {
    uint32_t st = sessionState;
    if (st == 0) {
        if (active) active = false;
        return;
    }
    if (st == 1) { powerLost = true; return; }
    if (!__sync_bool_compare_and_swap(&sessionState, 2u, 3u)) return;

    active = false;
    __sync_synchronize();
    for (uint32_t i = 0; i < 20 && inFlight; i++) IOSleep(1);
    if (pipelineReady || triggered) stopStream();
    pipelineReady = false;
    triggered = false;
    dspInit = false;
    sessionState = 0;
    log("stream stopped (%s); DSP initialized %d", why, bootDone);
    if (host) host->stateChanged();
}

bool IntelDspAudioEngine::initializeDsp() {
    if (!host || !imicEngine) return false;
    if (isAlive()) {
        dspBootState = 2;
        return true;
    }

    // Single flight: only one firmware boot may run at a time. Callers that lose the race wait
    // for the owner instead of touching the loader stream / core registers themselves.
    if (!__sync_bool_compare_and_swap(&dspBootState, 0u, 1u)) {
        for (uint32_t i = 0; i < 3000 && dspBootState == 1; i++) IOSleep(1);
        return isAlive();
    }

    bool ok = false;
    for (uint32_t attempt = 0; attempt < 2 && !ok; attempt++) {
        if (attempt) IOSleep(10);
        if (!host->holdPower()) {
            log("DSP init: controller power unavailable");
            break;
        }
        if (!probe() || !ensureDspMapped()) {
            log("DSP init: controller not ready");
            host->releasePower();
            continue;
        }
        ok = bootFirmware();
        if (!ok) host->releasePower();
    }

    dspBootState = ok ? 2u : 0u;
    if (ok) {
        log("DSP initialized, core stays powered for the engine lifetime");
        if (host) host->stateChanged();
    }
    return ok;
}

void IntelDspAudioEngine::powerDownSession(const char *why) {
    if (sessionState == 1) {
        powerLost = true;
        for (uint32_t i = 0; i < 2000 && sessionState == 1; i++) IOSleep(1);
    }
    stopStreaming(why);
    if (bootDone && dsp()) {
        corePowerDownInternal();
        gating(true);
        bootDone = false;
    }
    if (host) host->releasePower();
    bootDone = false;
    dspBootState = 0;
    log("DSP powered down (%s)", why);
    if (host) host->stateChanged();
}

// ---- firmware / pipeline ------------------------------------------------------------------

bool IntelDspAudioEngine::bootFirmware() {
    if (bootDone && isAlive()) return true;
    if (!probe() || !ensureDspMapped()) return fail("controller not ready");
    if (!ppOffset()) return fail("no PP capability");

    uint32_t adspcs = d32(DSP::ADSPCS);
    if (adspcs == 0xFFFFFFFF) return fail("ADSPCS unreadable", adspcs);
    if (adspcs & CPA(CORE0)) {
        // Never reset a core that is serving an active session. A stray PM-driven re-init used to
        // power down the DSP underneath the first recording and corrupt it.
        if (active || sessionState == 2 || sessionState == 3)
            return fail("reset while streaming", sessionState);
        corePowerDownInternal();
    }

    uint32_t fwSize = 0;
    const uint8_t *fw = firmwareImage(fwSize);
    if (!fw || fwSize < 8) return fail("firmware image", fwSize);
    const uint32_t *w = reinterpret_cast<const uint32_t *>(fw);
    size_t skip = (w[0] == XMAN_MAGIC) ? w[1] : 0;
    if (skip >= fwSize) return fail("bad firmware manifest", (uint32_t)skip);
    size_t fwBytes = fwSize - skip;

    // Prefer the highest spare output stream, as AppleHDA normally starts from output 0.
    uint32_t outIdx = numOutputStreams ? numOutputStreams - 1 : 0;
    for (uint32_t i = numOutputStreams; i-- > 0;) {
        if (!(r32(sdOffset(numInputStreams + i)) & HDA::SD_CTL_RUN)) { outIdx = i; break; }
    }
    uint32_t idx = numInputStreams + outIdx, tag = outIdx + 1;
    if ((r32(sdOffset(idx)) & HDA::SD_CTL_RUN) && !(r32(ppOffset() + HDA::PP_PPCTL) & (1u << idx)))
        return fail("no spare output stream", idx);

    if (!clBuf.alloc(device(), fwBytes) || !bdlBuf.alloc(device(), 4096))
        return fail("DMA alloc");
    memcpy(clBuf.virt, fw + skip, fwBytes);

    uint16_t cmd = device()->configRead16(kIOPCIConfigCommand);
    bool forcedBme = false;
    if (!(cmd & kIOPCICommandBusMaster)) {
        device()->setBusMasterEnable(true);
        forcedBme = true;
    }
    uint8_t d0i3 = r8(VS_D0I3C);
    if (d0i3 & D0I3C_I3) w8(VS_D0I3C, d0i3 & ~D0I3C_I3);
    setNoSnoop(true);
    gating(false);

    bool ok = false;
    uint32_t v = 0;
    do {
        if (!loaderStream(idx, tag, fwBytes)) break;
        if (!corePowerUp()) break;
        dw32(DSP::HIPCIDR, DSP::IPC_BUSY | ROM_IPC_CONTROL | ROM_IPC_PURGE_FW | ((tag - 1) << 9));
        if (!coreRun()) break;
        if (!pollDsp(DSP::HIPCIDA, DSP::IPC_DONE, DSP::IPC_DONE, 500000, &v)) { fail("ROM IPC ack", v); break; }
        dw32(DSP::HIPCIDA, d32(DSP::HIPCIDA) | DSP::IPC_DONE);
        // cl_dsp_init step 5: the ROM and this pipeline run on core 0 only; power the other
        // host-managed cores (cAVS 1.8: 1..3) back down, as Linux hda_dsp_core_reset_power_down() does.
        if (uint32_t others = targetCoreMask() & ~CORE0) coreResetPowerDown(others);
        if (!pollDsp(DSP::ROM_STATUS, FSR_MASK, FSR_INIT_DONE, 300000, &v)) { fail("ROM init", v); break; }
        uint32_t sd = sdOffset(idx);
        w32(sd, r32(sd) | HDA::SD_CTL_RUN | SD_CTL_INT_MASK);
        uint32_t lastFsr = d32(DSP::ROM_STATUS), t = 0;
        while (t < 3000000) {
            IODelay(20); t += 20;
            uint32_t f = d32(DSP::ROM_STATUS);
            if (f == lastFsr) continue;
            lastFsr = f;
            if ((f & FSR_MASK) == FSR_FW_ENTERED || (f & 0x80000000u)) break;
        }
        bool entered = pollDsp(DSP::ROM_STATUS, FSR_MASK, FSR_FW_ENTERED, 1000, &v);
        w32(sd, r32(sd) & ~(HDA::SD_CTL_RUN | SD_CTL_INT_MASK));
        if (!entered) { fail("FW_ENTERED", v); break; }
        if (!pollDsp(DSP::HIPCTDR, DSP::IPC_BUSY, DSP::IPC_BUSY, 1000000, &v)) { fail("FW_READY", v); break; }
        uint32_t msg = v & 0x7FFFFFFF;
        dw32(DSP::HIPCTDR, d32(DSP::HIPCTDR) | DSP::IPC_BUSY);
        dw32(DSP::HIPCTDA, d32(DSP::HIPCTDA) | DSP::IPC_DONE);
        if (msg != DSP::FW_READY) { fail("first DSP message", msg); break; }
        ok = true;
    } while (false);

    releaseLoaderStream(idx);
    clBuf.release();
    bdlBuf.release();
    if (ok) {
        bootDone = true;
        snprintf(errorText, sizeof(errorText), "OK FW_READY");
        log("firmware running (FW_READY)");
    } else {
        if (!active && sessionState != 2 && sessionState != 3) {
            corePowerDownInternal();
            gating(true);
        }
        if (forcedBme) device()->setBusMasterEnable(false);
        if (d0i3 & D0I3C_I3) w8(VS_D0I3C, d0i3);
    }
    return ok;
}

bool IntelDspAudioEngine::ensureBuffers() {
    if (ringBuf.mem && ptBuf.mem && capBdl.mem) return true;
    releaseBuffers();
    if (ringBuf.alloc(device(), kRingBytes) && ptBuf.alloc(device(), 4096) &&
        capBdl.alloc(device(), 4096))
        return true;
    releaseBuffers();
    return false;
}

void IntelDspAudioEngine::releaseBuffers() {
    ringBuf.release();
    ptBuf.release();
    capBdl.release();
}

bool IntelDspAudioEngine::prepareIpcStream(uint32_t idx, uint32_t tag) {
    uint32_t sd = sdOffset(idx);
    uint8_t *pt = ptBuf.virt;
    for (uint32_t i = 0; i < PAGES; i++) {
        uint32_t pfn = (uint32_t)((ringBuf.iova + i * 4096ULL) >> 12);
        uint8_t *p = pt + ((5 * i) >> 1);
        uint32_t v = (i & 1) ? ((p[0] & 0xF) | (pfn << 4)) : pfn;
        p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
    }
    struct Bdle { uint32_t lo, hi, len, ioc; };
    auto bdl = reinterpret_cast<Bdle *>(capBdl.virt);
    for (uint32_t i = 0; i < kRingBytes / kPeriodBytes; i++) {
        uint64_t a = ringBuf.iova + i * kPeriodBytes;
        bdl[i] = { (uint32_t)a, (uint32_t)(a >> 32), kPeriodBytes, 0 };
    }
    ptBuf.sync(); capBdl.sync();

    w32(ppOffset() + HDA::PP_PPCTL, r32(ppOffset() + HDA::PP_PPCTL) | (1u << idx));
    if (spbCapOffset)
        w32(spbCapOffset + SPB_SPBFCCTL, r32(spbCapOffset + SPB_SPBFCCTL) & ~(1u << idx));
    if (!streamStopReset(sd)) return fail("capture stream reset", r32(sd));
    w32(sd + HDA::SD_BDPL, 0); w32(sd + HDA::SD_BDPU, 0);
    w32(sd, (r32(sd) & ~SD_TAG_MASK) | (tag << HDA::SD_CTL_TAG_SHIFT));
    w32(sd + HDA::SD_CBL, kRingBytes);
    w16(sd + HDA::SD_FMT, (uint16_t)(FMT_48K_32BIT | (srcChannels - 1)));
    w16(sd + HDA::SD_LVI, kRingBytes / kPeriodBytes - 1);
    w32(sd + HDA::SD_BDPL, (uint32_t)capBdl.iova);
    w32(sd + HDA::SD_BDPU, (uint32_t)(capBdl.iova >> 32));
    return true;
}

bool IntelDspAudioEngine::startStream() {
    if (pipelineReady) return true;
    const uint32_t sd = sdOffset(captureIdx);
    if ((r32(sd) & HDA::SD_CTL_RUN) && !(r32(ppOffset() + HDA::PP_PPCTL) & (1u << captureIdx)))
        return fail("input stream in use by HDA driver", captureIdx);
    if (!ensureBuffers()) return fail("DMA alloc");
    bzero(ringBuf.virt, ringBuf.size);
    if (!prepareIpcStream(captureIdx, captureTag)) return false;
    if (!replay(ipc->setup, ipc->setupCount, "setup")) { stopStream(); return false; }
    pipelineReady = true;
    w32(sd, r32(sd) | HDA::SD_CTL_RUN | SD_CTL_INT_MASK);
    uint32_t v = 0;
    pollHda(sd, 0x20000000, 0x20000000, 5000, &v);
    if (!replay(ipc->start, ipc->startCount, "start")) { stopStream(); return false; }
    triggered = true;
    return true;
}

void IntelDspAudioEngine::stopStream() {
    const uint32_t sd = sdOffset(captureIdx);
    if (!dsp()) return;
    const bool alive = dspAlive(false);
    if (triggered && alive) replay(ipc->stop, 1, "stop");
    w32(sd, r32(sd) & ~(HDA::SD_CTL_RUN | SD_CTL_INT_MASK));
    pollHda(sd, HDA::SD_CTL_RUN, 0, 5000);
    if (pipelineReady && alive) {
        if (triggered) replay(ipc->stop + 1, ipc->stopCount - 1, "pcm-free");
        replay(ipc->teardown, ipc->teardownCount, "teardown");
    }
    streamStopReset(sd);
    w32(sd + HDA::SD_BDPL, 0); w32(sd + HDA::SD_BDPU, 0);
    w32(sd, r32(sd) & ~SD_TAG_MASK);
    w32(ppOffset() + HDA::PP_PPCTL, r32(ppOffset() + HDA::PP_PPCTL) & ~(1u << captureIdx));
    pipelineReady = triggered = false;
}

bool IntelDspAudioEngine::loaderStream(uint32_t idx, uint32_t tag, size_t fwBytes) {
    uint32_t sd = sdOffset(idx);
    w32(ppOffset() + HDA::PP_PPCTL, r32(ppOffset() + HDA::PP_PPCTL) | (1u << idx));
    if (!streamStopReset(sd)) return fail("loader reset", r32(sd));
    w32(sd + HDA::SD_BDPL, 0); w32(sd + HDA::SD_BDPU, 0);
    uint32_t half = (uint32_t)(fwBytes / 2) & ~0x7Fu;
    struct Bdle { uint32_t lo, hi, len, ioc; };
    auto bdl = reinterpret_cast<Bdle *>(bdlBuf.virt);
    bdl[0] = { (uint32_t)clBuf.iova, (uint32_t)(clBuf.iova >> 32), half, 0 };
    bdl[1] = { (uint32_t)(clBuf.iova + half), (uint32_t)((clBuf.iova + half) >> 32),
               (uint32_t)fwBytes - half, 0 };
    bdlBuf.sync(); clBuf.sync();
    w32(sd, (r32(sd) & ~SD_TAG_MASK) | (tag << HDA::SD_CTL_TAG_SHIFT));
    w32(sd + HDA::SD_CBL, (uint32_t)fwBytes);
    w16(sd + HDA::SD_FMT, CL_FORMAT);
    w16(sd + HDA::SD_LVI, 1);
    w32(sd + HDA::SD_BDPL, (uint32_t)bdlBuf.iova);
    w32(sd + HDA::SD_BDPU, (uint32_t)(bdlBuf.iova >> 32));
    if (spbCapOffset) {
        uint32_t spib = spbCapOffset + SPB_SPIB_BASE + SPB_SPIB_STRIDE * idx;
        w32(spbCapOffset + SPB_SPBFCCTL, r32(spbCapOffset + SPB_SPBFCCTL) | (1u << idx));
        w32(spib, (uint32_t)fwBytes);
    }
    return true;
}

void IntelDspAudioEngine::releaseLoaderStream(uint32_t idx) {
    uint32_t sd = sdOffset(idx);
    if (spbCapOffset) {
        w32(spbCapOffset + SPB_SPIB_BASE + SPB_SPIB_STRIDE * idx, 0);
        w32(spbCapOffset + SPB_SPBFCCTL, r32(spbCapOffset + SPB_SPBFCCTL) & ~(1u << idx));
    }
    streamStopReset(sd);
    w32(sd + HDA::SD_BDPL, 0); w32(sd + HDA::SD_BDPU, 0);
    w32(sd, r32(sd) & ~SD_TAG_MASK);
    w32(ppOffset() + HDA::PP_PPCTL, r32(ppOffset() + HDA::PP_PPCTL) & ~(1u << idx));
}

bool IntelDspAudioEngine::corePowerUp() {
    uint32_t mask = targetCoreMask();
    dw32(DSP::ADSPCS, d32(DSP::ADSPCS) | SPA(mask));
    uint32_t v = 0;
    if (!pollDsp(DSP::ADSPCS, CPA(mask), CPA(mask), 50000, &v)) {
        if (mask != CORE0 && (v & CPA(CORE0)) == CPA(CORE0)) return true;
        dw32(DSP::ADSPCS, (d32(DSP::ADSPCS) & ~SPA(mask)) | SPA(CORE0));
        if (!pollDsp(DSP::ADSPCS, CPA(CORE0), CPA(CORE0), 50000, &v))
            return fail("core power up", v);
    }
    return true;
}

bool IntelDspAudioEngine::coreRun() {
    dw32(DSP::ADSPCS, d32(DSP::ADSPCS) & ~CRST(CORE0));
    uint32_t v;
    if (!pollDsp(DSP::ADSPCS, CRST(CORE0), 0, 50000, &v)) return fail("core leave reset", v);
    dw32(DSP::ADSPCS, d32(DSP::ADSPCS) & ~CSTALL(CORE0));
    v = d32(DSP::ADSPCS);
    if ((v & (CPA(CORE0) | SPA(CORE0) | CRST(CORE0) | CSTALL(CORE0))) != (CPA(CORE0) | SPA(CORE0)))
        return fail("core not running", v);
    return true;
}

// Stall + reset the given cores, then remove their power (hda_dsp_core_reset_power_down). Not fatal.
void IntelDspAudioEngine::coreResetPowerDown(uint32_t mask) {
    dw32(DSP::ADSPCS, d32(DSP::ADSPCS) | CSTALL(mask));
    dw32(DSP::ADSPCS, d32(DSP::ADSPCS) | CRST(mask));
    pollDsp(DSP::ADSPCS, CRST(mask), CRST(mask), 50000);
    dw32(DSP::ADSPCS, d32(DSP::ADSPCS) & ~SPA(mask));
    uint32_t v = 0;
    bool ok = pollDsp(DSP::ADSPCS, CPA(mask), 0, 50000, &v);
    log("cores 0x%x %s, ADSPCS 0x%08x", mask, ok ? "powered down" : "did NOT power down", v);
}
void IntelDspAudioEngine::corePowerDownInternal() {
    if (!dsp()) return;
    uint32_t mask = targetCoreMask();
    dw32(DSP::ADSPCS, d32(DSP::ADSPCS) | CSTALL(CORE0));
    dw32(DSP::ADSPCS, d32(DSP::ADSPCS) | CRST(CORE0));
    pollDsp(DSP::ADSPCS, CRST(CORE0), CRST(CORE0), 50000);
    dw32(DSP::ADSPCS, d32(DSP::ADSPCS) & ~(SPA(mask) | SPA(CORE0)));
    uint32_t v = 0;
    pollDsp(DSP::ADSPCS, CPA(CORE0), 0, 50000, &v);
    setNoSnoop(false);
}

bool IntelDspAudioEngine::dspAlive(bool logReason) {
    bool alive = isAlive();
    if (!alive && logReason)
        log("DSP not alive (ADSPCS 0x%08x, GCTL 0x%08x, boot %d)",
            dsp() ? d32(DSP::ADSPCS) : 0, hda() ? r32(HDA::GCTL) : 0, bootDone);
    return alive;
}

// ---- IPC ----------------------------------------------------------------------------------

void IntelDspAudioEngine::ackDspNotification() {
    uint32_t tdr = d32(DSP::HIPCTDR);
    if (!(tdr & DSP::IPC_BUSY)) return;
    dw32(DSP::HIPCTDR, tdr | DSP::IPC_BUSY);
    dw32(DSP::HIPCTDA, d32(DSP::HIPCTDA) | DSP::IPC_DONE);
}

bool IntelDspAudioEngine::ipcSend(const uint32_t *msg, uint32_t words, uint32_t *reply, uint32_t replyWords) {
    if (d32(DSP::HIPCIDR) & DSP::IPC_BUSY) return false;
    for (uint32_t i = 0; i < words; i++) dw32(DSP::MBOX_DOWN + 4 * i, msg[i]);
    dw32(DSP::HIPCIDR, DSP::IPC_BUSY);
    uint32_t a = 0;
    for (uint32_t t = 0; t < 500000; t += 10) {
        a = d32(DSP::HIPCIDA);
        if (a & DSP::IPC_DONE) break;
        ackDspNotification();
        IODelay(10);
    }
    if (!(a & DSP::IPC_DONE)) return false;
    int32_t err = (int32_t)d32(DSP::MBOX_DOWN + 8);
    for (uint32_t i = 0; reply && i < replyWords; i++) reply[i] = d32(DSP::MBOX_DOWN + 4 * i);
    dw32(DSP::HIPCIDA, a | DSP::IPC_DONE);
    return err >= 0;
}

bool IntelDspAudioEngine::replay(const IpcMsg *list, uint32_t count, const char *phase) {
    for (uint32_t i = 0; i < count; i++) {
        const IpcMsg &m = list[i];
        uint32_t buf[64];
        if (m.words > COUNT(buf)) return fail("IPC message too large", m.words);
        memcpy(buf, m.data, m.words * 4);
        uint32_t reply[5] {};
        if (m.patch == kPatchPcmParams) {
            buf[kPcmParamPhy] = (uint32_t)ptBuf.iova;
            buf[kPcmParamPages] = PAGES;
            buf[kPcmParamSize] = kRingBytes;
            buf[kPcmParamTagCh] = (srcChannels << 16) | captureTag;
            buf[kPcmParamPeriod] = kPeriodBytes;
        }
        if (!ipcSend(buf, m.words, reply, 5)) {
            char what[48];
            snprintf(what, sizeof(what), "%s[%u] %s", phase, i, m.name);
            return fail(what, m.cmd);
        }
    }
    return true;
}

// ---- sample path --------------------------------------------------------------------------

bool IntelDspAudioEngine::fillFromRing(void *destBuf, uint32_t firstFrame, uint32_t numFrames,
                                  const IOAudioStreamFormat *format) {
    if (!ringBuf.virt) { fillFailMask |= 1u << 0; return false; }
    if (!engineFrameBytes || !engineRingFrames) { fillFailMask |= 1u << 1; return false; }
    if (!hda()) { fillFailMask |= 1u << 2; return false; }

    uint32_t sd = sdOffset(engineDesc);
    uint32_t ctl = r32(sd);
    uint32_t lpib = r32(sd + HDA::SD_LPIB);
    uint32_t dpibRaw = r32(dpibOffset(captureIdx));
    bool running = ctl != 0xFFFFFFFF && (ctl & HDA::SD_CTL_RUN);
    bool tagOk = ctl != 0xFFFFFFFF &&
                 (((ctl >> HDA::SD_CTL_TAG_SHIFT) & 0xF) == imicTagValue);
    if (!running || !tagOk) {
        // AppleHDA may move the input stream descriptor between starts. Find the running one with
        // the internal mic tag once, then use it for this and later conversions.
        uint32_t newDesc = 0; uint16_t newFmt = 0; uint32_t newCbl = 0;
        if (findRunningDescriptorByTag(imicTagValue, newDesc, newFmt, newCbl)) {
            engineDesc = newDesc;
            syncGeometry(newFmt, newCbl);
            sd = sdOffset(engineDesc);
            ctl = r32(sd);
            lpib = r32(sd + HDA::SD_LPIB);
            dpibRaw = r32(dpibOffset(captureIdx));
            running = ctl != 0xFFFFFFFF && (ctl & HDA::SD_CTL_RUN);
            tagOk = ctl != 0xFFFFFFFF &&
                    (((ctl >> HDA::SD_CTL_TAG_SHIFT) & 0xF) == imicTagValue);
        }
    }
    if (!running) { fillFailMask |= 1u << 3; return false; }
    if (!tagOk) { fillFailMask |= 1u << 4; return false; }
    if (lpib == 0xFFFFFFFF) { fillFailMask |= 1u << 5; return false; }
    if (dpibRaw == 0xFFFFFFFF) { fillFailMask |= 1u << 6; return false; }

    uint16_t fmt = *reinterpret_cast<volatile uint16_t *>(hda() + sd + HDA::SD_FMT);
    uint32_t cbl = r32(sd + HDA::SD_CBL);
    if (fmt && cbl && (fmt != lastFmt || cbl != lastCbl)) syncGeometry(fmt, cbl);

    uint32_t ch = format ? format->fNumChannels : 2;
    if (!ch || ch > 8) ch = 2;
    bool toFloat = format ? (format->fIsMixable != 0) : true;
    if (numFrames > engineRingFrames) numFrames = engineRingFrames;

    const uint32_t ringBytes = (uint32_t)ringBuf.size;
    const uint32_t srcFrames = ringBytes / srcFrameBytes;
    if (!srcFrames) { fillFailMask |= 1u << 7; return false; }
    const uint32_t W = (lpib / engineFrameBytes) % engineRingFrames;

    const uint64_t wrapQ = (uint64_t)srcFrames << 32;
    uint64_t dpibQ = (uint64_t)((dpibRaw % ringBytes) / srcFrameBytes) << 32;
    uint64_t now = mach_absolute_time();
    if (!dspInit) {
        dspQ = dpibQ; dspTime = now; dspInit = true;
    } else {
        uint64_t dns = 0;
        absolutetime_to_nanoseconds(now - dspTime, &dns);
        if (dns > 200000000ULL) {
            dspQ = dpibQ;
        } else {
            __uint128_t framesQ = ((__uint128_t)dns * 48000) << 32;
            dspQ = (dspQ + (uint64_t)(framesQ / 1000000000ULL)) % wrapQ;
            int64_t delta = (int64_t)((dpibQ + wrapQ - dspQ) % wrapQ);
            if (delta > (int64_t)(wrapQ >> 1)) delta -= (int64_t)wrapQ;
            int64_t limit = (int64_t)2048 << 32;
            if (delta > limit || delta < -limit) dspQ = dpibQ;
        }
        dspTime = now;
    }
    const uint64_t headQ = dspQ;
    const uint64_t marginQ = (uint64_t)256 << 32;

    uint32_t first = firstFrame % engineRingFrames;
    uint32_t dFirst = (W + engineRingFrames - first) % engineRingFrames;
    if (dFirst > engineRingFrames / 2) dFirst = 0;
    uint64_t distFirst = (uint64_t)dFirst * engineStep + marginQ;
    uint32_t need = (uint32_t)((distFirst >> 32) + (((uint64_t)numFrames * engineStep) >> 32) + 4);
    if (need > srcFrames) need = srcFrames;
    uint32_t start = (uint32_t)(((headQ + wrapQ - (distFirst % wrapQ)) % wrapQ) >> 32);
    uint32_t startByte = start * srcFrameBytes, bytes = need * srcFrameBytes;
    if (startByte + bytes <= ringBytes) {
        ringBuf.invalidate(startByte, bytes);
    } else {
        ringBuf.invalidate(startByte, ringBytes - startByte);
        ringBuf.invalidate(0, bytes - (ringBytes - startByte));
    }

    const int32_t *src = reinterpret_cast<const int32_t *>(ringBuf.virt);
    const uint32_t gain = gainQ16;
    uint32_t p = first;
    for (uint32_t i = 0; i < numFrames; i++) {
        uint32_t d = (W + engineRingFrames - p) % engineRingFrames;
        if (d > engineRingFrames / 2) d = 0;
        uint64_t off = (uint64_t)d * engineStep + marginQ;
        uint64_t q = (headQ + wrapQ - (off % wrapQ)) % wrapQ;
        uint32_t si = (uint32_t)(q >> 32), frac = (uint32_t)q;
        const int32_t *a = src + si * srcChannels;
        const int32_t *b = src + ((si + 1) % srcFrames) * srcChannels;
        int32_t s[4] {};
        for (uint32_t c = 0; c < srcChannels && c < 4; c++) {
            int64_t v;
            if (frac == 0) v = a[c];
            else v = (int64_t)a[c] + (((int64_t)(b[c] - a[c]) * (int64_t)frac) >> 32);
            if (gain != 65536) v = (v * (int64_t)gain) >> 16;
            if (v > 2147483647LL) v = 2147483647LL;
            if (v < -2147483648LL) v = -2147483648LL;
            s[c] = (int32_t)v;
        }
        if (toFloat) {
            float *out = reinterpret_cast<float *>(destBuf) + (uint64_t)i * ch;
            for (uint32_t c = 0; c < ch; c++) {
                int32_t v = (ch == 1) ? (int32_t)(((int64_t)s[0] + s[1]) / 2) : s[chanMap[c & 7]];
                out[c] = (float)v * (1.0f / 2147483648.0f);
            }
        } else if (engineBits == 16) {
            int16_t *out = reinterpret_cast<int16_t *>(destBuf) + (uint64_t)i * ch;
            for (uint32_t c = 0; c < ch; c++) {
                int32_t v = (ch == 1) ? (int32_t)(((int64_t)s[0] + s[1]) / 2) : s[chanMap[c & 7]];
                out[c] = (int16_t)(v >> 16);
            }
        } else {
            int32_t *out = reinterpret_cast<int32_t *>(destBuf) + (uint64_t)i * ch;
            for (uint32_t c = 0; c < ch; c++)
                out[c] = (ch == 1) ? (int32_t)(((int64_t)s[0] + s[1]) / 2) : s[chanMap[c & 7]];
        }
        p = (p + 1) % engineRingFrames;
    }
    return true;
}
