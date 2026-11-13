// Original AppleHDA function pointers captured by the Lilu routes.
//
// Every pointer keeps the exact AppleHDA ABI: the first argument is AppleHDA's own object
// (AppleHDAEngineInput) and is passed through unchanged by HdaAudioEngine.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/audio/IOAudioTypes.h>

class IOAudioStream;

struct NativeHdaOps {
    bool     (*initHardware)(IOService *self, IOService *provider) {nullptr};
    IOReturn (*performAudioEngineStart)(IOService *self) {nullptr};
    IOReturn (*performAudioEngineStop)(IOService *self) {nullptr};
    void     (*stopHardware)(IOService *self, IOService *provider) {nullptr};
    IOReturn (*performFormatChange)(IOService *self, IOAudioStream *stream,
                                    const IOAudioStreamFormat *format,
                                    const IOAudioSampleRate *rate) {nullptr};
    IOReturn (*convertInputSamples)(IOService *self, const void *sampleBuf, void *destBuf,
                                    UInt32 firstFrame, UInt32 numFrames,
                                    const IOAudioStreamFormat *format,
                                    IOAudioStream *stream) {nullptr};
};
