// Lilu routing and static AppleHDA trampolines.
//
// Hook targets must be static functions with the exact AppleHDA ABI. They forward to the Manager
// (if one exists) or call the captured original function pointers directly.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <mach/mach_types.h>
#include "NativeHdaOps.hpp"

class KernelPatcher;
class DspAudioEngineManager;
class IOAudioStream;

class HookManager {
public:
    struct Status {
        uint32_t kmodEntry {0}, reqAccess {0}, shouldLoad {0}, started {0};
        uint32_t arg {0}, onKextErr {0}, kextCb {0}, routeOk {0};
        uint32_t convertError {0}, engineError {0};
        uint32_t kextId {0}, kextSize {0}, solveOk {0}, solveError {0};
        uint64_t kextAddr {0}, solveAddr {0};
        // bit0 init, bit1 performStart, bit2 performStop, bit3 stopHardware, bit4 format
        uint32_t routeEngine {0};
    };

    static bool install(KernelPatcher &patcher, size_t kextId,
                        mach_vm_address_t address, size_t size);

    static void setManager(DspAudioEngineManager *m);
    static DspAudioEngineManager *manager() { return currentManager; }
    static const NativeHdaOps &nativeOps() { return ops; }
    static Status &status() { return routeStatus; }

private:
    static bool initHardwareHook(IOService *self, IOService *provider);
    static IOReturn performStartHook(IOService *self);
    static IOReturn performStopHook(IOService *self);
    static void stopHardwareHook(IOService *self, IOService *provider);
    static IOReturn formatChangeHook(IOService *self, IOAudioStream *stream,
                                     const IOAudioStreamFormat *format,
                                     const IOAudioSampleRate *rate);
    static IOReturn convertInputHook(IOService *self, const void *sampleBuf, void *destBuf,
                                     UInt32 firstFrame, UInt32 numFrames,
                                     const IOAudioStreamFormat *format, IOAudioStream *stream);

    static NativeHdaOps ops;
    static Status routeStatus;
    static DspAudioEngineManager *currentManager;
};
