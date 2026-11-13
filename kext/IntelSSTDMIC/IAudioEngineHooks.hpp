// Hook entry points.
//
// The parameter lists mirror the AppleHDAEngine / AppleHDAEngineInput methods that Lilu routes.
// The AppleHDA object is passed explicitly as `self`; our classes are never that object.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/audio/IOAudioTypes.h>
#include "NativeHdaOps.hpp"

class IOAudioStream;

class IAudioEngineHooks {
public:
    virtual ~IAudioEngineHooks() {}

    // Allows HookManager to inject the original function pointers after routing.
    virtual void setNativeOps(const NativeHdaOps *ops) = 0;

    virtual bool initHardware(IOService *self, IOService *provider) = 0;
    virtual IOReturn performAudioEngineStart(IOService *self) = 0;
    virtual IOReturn performAudioEngineStop(IOService *self) = 0;
    virtual void stopHardware(IOService *self, IOService *provider) = 0;
    virtual IOReturn performFormatChange(IOService *self, IOAudioStream *stream,
                                         const IOAudioStreamFormat *format,
                                         const IOAudioSampleRate *rate) = 0;
    virtual IOReturn convertInputSamples(IOService *self, const void *sampleBuf, void *destBuf,
                                         UInt32 firstFrame, UInt32 numFrames,
                                         const IOAudioStreamFormat *format,
                                         IOAudioStream *stream) = 0;
};
