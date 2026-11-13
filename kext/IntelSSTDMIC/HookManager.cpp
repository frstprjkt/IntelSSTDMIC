#include "HookManager.hpp"
#include "DspAudioEngineManager.hpp"
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/audio/IOAudioStream.h>

NativeHdaOps HookManager::ops {};
HookManager::Status HookManager::routeStatus {};
DspAudioEngineManager *HookManager::currentManager = nullptr;

namespace {
constexpr const char *kConvertInputSymbol =
    "__ZN14AppleHDAEngine19convertInputSamplesEPKvPvjjPK20_IOAudioStreamFormatP13IOAudioStream";
constexpr const char *kInitHardwareSymbol =
    "__ZN19AppleHDAEngineInput12initHardwareEP9IOService";
constexpr const char *kStopHardwareSymbol =
    "__ZN19AppleHDAEngineInput12stopHardwareEP9IOService";
constexpr const char *kPerformStartSymbol =
    "__ZN14AppleHDAEngine23performAudioEngineStartEv";
constexpr const char *kPerformStopSymbol =
    "__ZN19AppleHDAEngineInput22performAudioEngineStopEv";
constexpr const char *kFormatChangeSymbol =
    "__ZN19AppleHDAEngineInput19performFormatChangeEP13IOAudioStreamPK20_IOAudioStreamFormatPK18_IOAudioSampleRate";
}

bool HookManager::install(KernelPatcher &patcher, size_t kextId,
                          mach_vm_address_t address, size_t size) {
    routeStatus.kextId = (uint32_t)kextId;
    routeStatus.kextSize = (uint32_t)size;
    routeStatus.kextAddr = address;

    // Diagnostic: resolve the data symbol directly. On kernel-collection boots the callback's
    // address/size range is not always a valid symbol search range.
    mach_vm_address_t solved = patcher.solveSymbol(kextId, kConvertInputSymbol);
    routeStatus.solveError = (uint32_t)patcher.getError();
    patcher.clearError();
    routeStatus.solveOk = solved != 0;
    routeStatus.solveAddr = solved;

    auto tryRoute = [&](KernelPatcher::RouteRequest &request) -> bool {
        bool ok = patcher.routeMultiple(kextId, &request, 1, address, size);
        uint32_t err = (uint32_t)patcher.getError();
        patcher.clearError();
        if (!ok) {
            ok = patcher.routeMultiple(kextId, &request, 1);
            err = (uint32_t)patcher.getError();
            patcher.clearError();
        }
        if (!ok) {
            ok = patcher.routeMultiple(kextId, &request, 1, 0, 0, true, true);
            err = (uint32_t)patcher.getError();
            patcher.clearError();
        }
        routeStatus.convertError = err;
        return ok;
    };

    bool convertOk = routeStatus.routeOk == 1;
    if (!convertOk) {
        KernelPatcher::RouteRequest request(kConvertInputSymbol, convertInputHook, ops.convertInputSamples);
        convertOk = tryRoute(request);
        routeStatus.routeOk = convertOk ? 1 : 2;
        IOLog("IntelSSTDMIC: Lilu hook convertInputSamples %s (err %u, solve %u/%u)\n",
              convertOk ? "installed" : "FAILED", routeStatus.convertError,
              routeStatus.solveOk, routeStatus.solveError);
    }

    if (!convertOk) {
        if (currentManager) currentManager->setNativeOps(&ops);
        return false;
    }

    auto routeLifecycle = [&](const char *symbol, auto hook, auto &original, uint32_t bit) {
        if (routeStatus.routeEngine & bit) return;
        KernelPatcher::RouteRequest request(symbol, hook, original);
        bool ok = tryRoute(request);
        if (ok) routeStatus.routeEngine |= bit;
        IOLog("IntelSSTDMIC: Lilu hook %s %s (err %u)\n", symbol,
              ok ? "installed" : "FAILED", routeStatus.convertError);
    };

    routeLifecycle(kInitHardwareSymbol, initHardwareHook, ops.initHardware, 1u << 0);
    routeLifecycle(kPerformStartSymbol, performStartHook, ops.performAudioEngineStart, 1u << 1);
    routeLifecycle(kPerformStopSymbol, performStopHook, ops.performAudioEngineStop, 1u << 2);
    routeLifecycle(kStopHardwareSymbol, stopHardwareHook, ops.stopHardware, 1u << 3);
    routeLifecycle(kFormatChangeSymbol, formatChangeHook, ops.performFormatChange, 1u << 4);

    if (currentManager) currentManager->setNativeOps(&ops);
    return true;
}

void HookManager::setManager(DspAudioEngineManager *m) {
    currentManager = m;
    if (m) m->setNativeOps(&ops);
}

// ---- static AppleHDA trampolines ----------------------------------------------------------

bool HookManager::initHardwareHook(IOService *self, IOService *provider) {
    if (currentManager) return currentManager->initHardware(self, provider);
    return ops.initHardware ? ops.initHardware(self, provider) : false;
}

IOReturn HookManager::performStartHook(IOService *self) {
    if (currentManager) return currentManager->performAudioEngineStart(self);
    return ops.performAudioEngineStart ? ops.performAudioEngineStart(self) : kIOReturnUnsupported;
}

IOReturn HookManager::performStopHook(IOService *self) {
    if (currentManager) return currentManager->performAudioEngineStop(self);
    return ops.performAudioEngineStop ? ops.performAudioEngineStop(self) : kIOReturnUnsupported;
}

void HookManager::stopHardwareHook(IOService *self, IOService *provider) {
    if (currentManager) currentManager->stopHardware(self, provider);
    else if (ops.stopHardware) ops.stopHardware(self, provider);
}

IOReturn HookManager::formatChangeHook(IOService *self, IOAudioStream *stream,
                                       const IOAudioStreamFormat *format,
                                       const IOAudioSampleRate *rate) {
    if (currentManager) return currentManager->performFormatChange(self, stream, format, rate);
    return ops.performFormatChange ? ops.performFormatChange(self, stream, format, rate)
                                   : kIOReturnUnsupported;
}

IOReturn HookManager::convertInputHook(IOService *self, const void *sampleBuf, void *destBuf,
                                       UInt32 firstFrame, UInt32 numFrames,
                                       const IOAudioStreamFormat *format, IOAudioStream *stream) {
    if (currentManager)
        return currentManager->convertInputSamples(self, sampleBuf, destBuf, firstFrame,
                                                   numFrames, format, stream);
    return ops.convertInputSamples
        ? ops.convertInputSamples(self, sampleBuf, destBuf, firstFrame, numFrames, format, stream)
        : kIOReturnUnsupported;
}
