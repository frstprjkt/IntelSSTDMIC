#include "IntelSSTDMIC.hpp"
#include "DspAudioEngineManager.hpp"
#include "HookManager.hpp"
#include "BootArgs.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <libkern/version.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSString.h>
#include <pexpert/pexpert.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <stdarg.h>

#define super IOService
OSDefineMetaClassAndStructors(IntelSSTDMIC, IOService)

IntelSSTDMIC *IntelSSTDMIC::gInstance = nullptr;

static constexpr unsigned kLogLines = 128;

static bool bootArg(const char *name) {
    return BootArgs::present(name[0] == '-' ? name + 1 : name);
}

static IOPMPowerState gPowerStates[2] = {
    { kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { kIOPMPowerStateVersion1, kIOPMPowerOn | kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};

void IntelSSTDMIC::logLine(const char *fmt, ...) {
    char buf[200];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    if (logLock) IOLockLock(logLock);
    IOLog("IntelSSTDMIC: %s\n", buf);
    if (!logRing) logRing = OSArray::withCapacity(kLogLines);
    if (logRing) {
        if (logRing->getCount() >= kLogLines) logRing->removeObject(0);
        if (auto s = OSString::withCString(buf)) { logRing->setObject(s); s->release(); }
    }
    if (logLock) IOLockUnlock(logLock);
}

bool IntelSSTDMIC::memoryDecoded() const {
    return pci && (pci->configRead16(kIOPCIConfigCommand) & kIOPCICommandMemorySpace);
}

bool IntelSSTDMIC::inD0() const {
    if (!pci) return false;
    uint8_t pm = 0;
    if (!pci->findPCICapability(kIOPCIPowerManagementCapability, &pm)) return true;
    return (pci->configRead16(pm + 4) & 0x3) == 0;
}

void IntelSSTDMIC::detectPlatform(uint16_t device) {
    platformFamily = PlatformFamily::Unknown;
    switch (device) {
        case 0x02C8: case 0x9DC8: case 0x06C8: case 0xA348:
            platformFamily = PlatformFamily::CometLake; break;
        case 0xA0C8: case 0x43C8:
            platformFamily = PlatformFamily::TigerLake; break;
        case 0x34C8:
            platformFamily = PlatformFamily::CometLake; break;
        default: break;
    }
    if (bootArg("-sstdmiccml")) platformFamily = PlatformFamily::CometLake;
    else if (bootArg("-sstdmictgl")) platformFamily = PlatformFamily::TigerLake;
}

static bool parseLayoutArg(const char *text, int32_t &value) {
    if (!text || !*text) return false;
    bool neg = false;
    const char *p = text;
    if (*p == '-') { neg = true; p++; }
    if (*p < '0' || *p > '9') return false;
    uint64_t v = 0;
    for (; *p >= '0' && *p <= '9'; p++) {
        v = v * 10 + (uint64_t)(*p - '0');
        if (v > 0x7FFFFFFFULL) return false;
    }
    if (*p) return false;
    value = neg ? -(int32_t)v : (int32_t)v;
    return true;
}

const LayoutConfig *IntelSSTDMIC::selectLayout() {
    layoutDisabled = false;
    layoutRequested = false;

    char arg[32] {};
    bool hasArg = BootArgs::find("sstdmiclayout", arg, sizeof(arg));
    layoutRequested = hasArg;
    int32_t argValue = 0;
    bool autoMode = false;
    if (!hasArg) {
        logLine("no -sstdmiclayout argument - DSP disabled, native HDA only");
        return &LayoutRegistry::none();
    }
    if (!strcmp(arg, "auto")) {
        autoMode = true;
    } else if (!parseLayoutArg(arg, argValue)) {
        logLine("invalid -sstdmiclayout value '%s' - native fallback", arg);
        return &LayoutRegistry::none();
    }

    char profile[48] {};
    BootArgs::find("sstdmicprofile", profile, sizeof(profile));
    char model[64] {};
    BootArgs::find("sstdmicmodel", model, sizeof(model));

    char fw[16] {};
    if (BootArgs::find("sstdmicfw", fw, sizeof(fw))) {
        if (!strcmp(fw, "tgl") || !strcmp(fw, "tigerlake")) platformFamily = PlatformFamily::TigerLake;
        else if (!strcmp(fw, "cml") || !strcmp(fw, "cnl") || !strcmp(fw, "cometlake"))
            platformFamily = PlatformFamily::CometLake;
    }

    if (!autoMode) {
        if (argValue == 0) {
            layoutDisabled = true;
            logLine("layout disabled (0) - native HDA only");
            return &LayoutRegistry::none();
        }
        if (argValue < 0) {
            logLine("invalid -sstdmiclayout value %d - native fallback", argValue);
            return &LayoutRegistry::none();
        }
        const LayoutConfig *p = registry.find((uint32_t)argValue);
        if (!p) {
            logLine("layout %d not found - native fallback", argValue);
            return &LayoutRegistry::none();
        }
        logLine("layout %u selected (%s, %s, engine %s, firmware %s)",
                p->id, p->name, p->platform == PlatformFamily::TigerLake ? "TGL" :
                p->platform == PlatformFamily::CometLake ? "CML" : "?", p->engine, p->firmware);
        return p;
    }

    // Auto: optional named layout/model first, then the detected platform default.
    const LayoutConfig *p = registry.resolve(-1, profile[0] ? profile : nullptr,
                                             model[0] ? model : nullptr, platformFamily);
    if (p && p->valid)
        logLine("layout %u auto-selected (%s, %s, engine %s, firmware %s)",
                p->id, p->name, p->platform == PlatformFamily::TigerLake ? "TGL" :
                p->platform == PlatformFamily::CometLake ? "CML" : "?", p->engine, p->firmware);
    else
        logLine("no layout available for auto detection (profile '%s', model '%s') - native fallback",
                profile, model);
    return p ? p : &LayoutRegistry::none();
}

IOService *IntelSSTDMIC::probe(IOService *provider, SInt32 *score) {
    if (bootArg("-sstdmicoff")) return nullptr;
    auto dev = OSDynamicCast(IOPCIDevice, provider);
    if (!dev) return nullptr;

    pciId = (uint32_t)dev->configRead32(kIOPCIConfigVendorID);
    uint16_t vendor = pciId & 0xFFFF;
    uint16_t device = (pciId >> 16) & 0xFFFF;
    if (vendor != 0x8086) {
        IOLog("IntelSSTDMIC: probe: unexpected PCI vendor %04x, not attaching\n", vendor);
        return nullptr;
    }
    detectPlatform(device);
    if (platformFamily == PlatformFamily::Unknown) {
        if (bootArg("-sstdmicforce")) {
            platformFamily = PlatformFamily::CometLake;
            IOLog("IntelSSTDMIC: probe: forced attach on unlisted PCI id 8086:%04x\n", device);
        } else {
            IOLog("IntelSSTDMIC: probe: unrecognised PCI id 8086:%04x, not attaching\n", device);
            return nullptr;
        }
    }
    IOLog("IntelSSTDMIC: probe: attaching to PCI 8086:%04x\n", device);
    return super::probe(provider, score);
}

bool IntelSSTDMIC::start(IOService *provider) {
    if (!super::start(provider)) return false;
    pci = OSDynamicCast(IOPCIDevice, provider);
    if (!pci) return false;
    pci->retain();

    logLock = IOLockAlloc();
    pciId = (uint32_t)pci->configRead32(kIOPCIConfigVendorID);
    detectPlatform((uint16_t)((pciId >> 16) & 0xFFFF));

    layout = selectLayout();
    injectorEnabled = layoutRequested && !layoutDisabled;

    if (!layoutRequested) {
        logLine("no -sstdmiclayout: driver stays on the native AppleHDA path");
    } else {
        manager = new DspAudioEngineManager;
        if (manager && !manager->init(&factory, this)) {
            manager->release();
            manager = nullptr;
        }
        if (!manager) {
            logLine("cannot create the DSP event loop/manager - native path only");
        } else {
            int32_t gain = layout->valid ? (int32_t)layout->gainPercent : 100;
            char gainArg[16] {};
            if (BootArgs::find("sstdmicgain", gainArg, sizeof(gainArg)) && gainArg[0]) {
                int32_t parsed = 0;
                if (parseLayoutArg(gainArg, parsed)) gain = parsed;
            }
            if (gain < 0) gain = 0;
            if (gain > 400) gain = 400;

            DspEngineContext ctx { *layout, pci, *this, *manager, HookManager::nativeOps(), (uint32_t)gain };
            DspEngineCreateRequest request {
                layoutDisabled ? DspEngineCreateSource::Disabled : DspEngineCreateSource::Layout,
                layout, platformFamily };
            manager->initialize(request, ctx);
            HookManager::setManager(manager);
        }
    }

    if (injectorEnabled) joinPowerParent();

    logLine("v%s on %s (Darwin %d.%d), injector %s, layout %u, engine %s",
            kIntelSSTDMICVersion, provider->getName(), version_major, version_minor,
            injectorEnabled ? "ON" : "off", layout ? layout->id : 0,
            manager ? manager->engineName() : "none");
    publish();
    gInstance = this;
    registerService();
    return true;
}

void IntelSSTDMIC::joinPowerParent() {
    if (pmJoined || !injectorEnabled || !pci) return;
    IOService *parent = nullptr;
    if (OSIterator *it = pci->getChildIterator(gIOServicePlane)) {
        while (OSObject *o = it->getNextObject()) {
            IOService *s = OSDynamicCast(IOService, o);
            if (!s || s == this || s->isInactive()) continue;
            const char *cn = s->getMetaClass()->getClassName();
            if (cn && strnstr(const_cast<char *>(cn), "HDA", 64)) { parent = s; break; }
        }
        it->release();
    }
    if (!parent) {
        if (++pmSearchTicks == 30) {
            logLine("PM: no HDA driver on this controller - no power hold");
            pmSearchTicks = 1000;
        }
        return;
    }
    const char *pcn = parent->getMetaClass()->getClassName();
    int pmArg = -1;
    PE_parse_boot_argn("sstdmicpower", &pmArg, sizeof(pmArg));
    bool join = pmArg == 1 || (pmArg != 0 && strnstr(const_cast<char *>(pcn), "AppleHDA", 64));
    if (!join) {
        logLine("PM: HDA driver is %s - not joining its power tree", pcn);
        pmSearchTicks = 1000;
        return;
    }
    PMinit();
    parent->joinPMtree(this);
    if (registerPowerDriver(this, gPowerStates, 2) != kIOReturnSuccess) {
        PMstop();
        logLine("PM: registerPowerDriver failed");
        pmSearchTicks = 1000;
        return;
    }
    pmParent = parent;
    pmParent->retain();
    pmJoined = true;
    changePowerStateTo(0);
    logLine("PM: power child of %s", pcn);
}

IOReturn IntelSSTDMIC::setPowerState(unsigned long state, IOService *) {
    unsigned long old = pmState;
    pmState = state;
    if (manager) {
        if (state == 0) {
            IOPMrootDomain *root = getPMRootDomain();
            bool systemSleep = root && root->getPowerState() == 0;
            if (systemSleep) {
                logLine("system sleep: stopping DSP session");
                manager->onPowerLostSync();
            } else if (!selfReleasingPower) {
                // The HDA driver idling its device power state must not stop the input session.
                logLine("device power 0 while system awake - keeping DSP session");
                changePowerStateTo(1);
            }
        } else if (state == 1) {
            logLine("power state 1 (wake): restoring DSP session if needed");
            manager->onPowerRestored();
        }
        if (state != old) publish();
    }
    return kIOPMAckImplied;
}

bool IntelSSTDMIC::holdPower() {
    if (!pmJoined && injectorEnabled) joinPowerParent();
    if (pmJoined && pmState != 1) changePowerStateTo(1);
    // Matches the original driver's power hold: wait up to 3 s for the controller to reach D0 and
    // decode memory. This now runs on the manager event loop, not inside an AppleHDA callback.
    for (uint32_t t = 0; t < 3000; t++) {
        if (memoryDecoded() && inD0()) {
            if (t) logLine("controller usable after %u ms", t);
            return true;
        }
        IOSleep(1);
    }
    logLine("controller not usable (CMD 0x%04x D0 %d)",
            pci ? pci->configRead16(kIOPCIConfigCommand) : 0, inD0());
    return false;
}

void IntelSSTDMIC::releasePower() {
    if (pmJoined) {
        selfReleasingPower = true;
        changePowerStateTo(0);
        selfReleasingPower = false;
    }
}

void IntelSSTDMIC::publish() {
    DspAudioEngineManager::Status st {};
    if (manager) st = manager->status();

    if (logLock) IOLockLock(logLock);
    setProperty("IntelSSTDMICVersion", kIntelSSTDMICVersion);
    setProperty("PlatformFamily", platformFamily == PlatformFamily::TigerLake ? "TigerLake" :
                                  platformFamily == PlatformFamily::CometLake ? "CometLake" : "Unknown");
    setProperty("PciVendorDeviceId", pciId, 32);
    setProperty("Injector", injectorEnabled);

    setProperty("LayoutId", layout ? layout->id : 0, 32);
    setProperty("LayoutName", layout ? layout->name : "");
    setProperty("EngineClass", st.engineName ? st.engineName : "none");
    if (st.createError) setProperty("CreateError", st.createError);
    setProperty("DspActive", st.dspActive);
    setProperty("LayoutDisabled", st.disabled);
    setProperty("LayoutRequested", layoutRequested);
    setProperty("LayoutCount", registry.count(), 32);
    setProperty("Ready", st.ready);
    setProperty("DspAlive", st.alive);
    setProperty("Streaming", st.streaming);
    setProperty("SetupDone", st.setupDone);
    setProperty("LastError", st.lastError ? st.lastError : "");
    setProperty("ImicTag", st.imicTag, 32);
    setProperty("ImicStreams", st.imicStreams, 32);
    setProperty("HookFills", st.fills, 64);
    setProperty("HookFrames", st.frames, 64);
    setProperty("HookCalls", st.hooks.calls, 32);
    setProperty("HookStreamOurs", st.hooks.streamOurs, 32);
    setProperty("HookEngineOurs", st.hooks.engineOurs, 32);
    setProperty("HookActive", st.hooks.activeCalls, 32);
    setProperty("HookInactive", st.hooks.inactive, 32);
    setProperty("HookFillOk", st.hooks.fillOk, 32);
    setProperty("HookFillFail", st.hooks.fillFail, 32);
    setProperty("FillFailMask", st.hooks.failMask, 32);
    setProperty("EventPosted", st.eventsPosted, 32);
    setProperty("EventCompleted", st.eventsCompleted, 32);
    setProperty("EventDropped", st.eventsDropped, 32);

    setProperty("HookKmodEntry", HookManager::status().kmodEntry, 32);
    setProperty("HookReqAccess", HookManager::status().reqAccess, 32);
    setProperty("HookShouldLoad", HookManager::status().shouldLoad, 32);
    setProperty("HookPluginStart", HookManager::status().started, 32);
    setProperty("HookArg", HookManager::status().arg, 32);
    setProperty("HookOnKextErr", HookManager::status().onKextErr, 32);
    setProperty("HookKextCb", HookManager::status().kextCb, 32);
    setProperty("HookRouteOk", HookManager::status().routeOk, 32);
    setProperty("HookRouteEngine", HookManager::status().routeEngine, 32);
    setProperty("HookConvertError", HookManager::status().convertError, 32);
    setProperty("HookEngineError", HookManager::status().engineError, 32);
    setProperty("HookKextId", HookManager::status().kextId, 32);
    setProperty("HookKextAddr", HookManager::status().kextAddr, 64);
    setProperty("HookKextSize", HookManager::status().kextSize, 32);
    setProperty("HookSolveOk", HookManager::status().solveOk, 32);
    setProperty("HookSolveError", HookManager::status().solveError, 32);
    setProperty("HookSolveAddr", HookManager::status().solveAddr, 64);

    if (logRing) if (auto copy = OSArray::withArray(logRing)) { setProperty("Log", copy); copy->release(); }
    if (logLock) IOLockUnlock(logLock);
}

void IntelSSTDMIC::stop(IOService *provider) {
    gInstance = nullptr;
    HookManager::setManager(nullptr);      // hooks must not reach the manager after this point
    if (manager) {
        manager->shutdown();
        manager->release();
        manager = nullptr;
    }
    if (pmJoined) { PMstop(); pmJoined = false; }
    OSSafeReleaseNULL(pmParent);
    OSSafeReleaseNULL(pci);
    if (logLock) IOLockLock(logLock);
    OSSafeReleaseNULL(logRing);
    if (logLock) { IOLockUnlock(logLock); IOLockFree(logLock); logLock = nullptr; }
    super::stop(provider);
}

void IntelSSTDMIC::free() {
    if (logLock) { IOLockLock(logLock); OSSafeReleaseNULL(logRing); IOLockUnlock(logLock); IOLockFree(logLock); logLock = nullptr; }
    else OSSafeReleaseNULL(logRing);
    OSSafeReleaseNULL(pci);
    super::free();
}
