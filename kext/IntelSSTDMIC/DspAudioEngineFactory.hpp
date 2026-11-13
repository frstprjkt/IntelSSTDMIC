// Factory: the single creation seam. Configuration-based creation is one strategy; future
// auto-detection or quirk engines only change this class. On failure it creates a native
// HdaAudioEngine so the hooks keep executing the original AppleHDA functions.
#pragma once
#include "IDspAudioEngine.hpp"

class IDspAudioEngineFactory {
public:
    virtual ~IDspAudioEngineFactory() {}

    virtual DspEngineCreateResult create(const DspEngineCreateRequest &request,
                                         const DspEngineContext &context) = 0;

    virtual void destroy(DspEngineCreateResult &result) = 0;
};

class DspAudioEngineFactory : public IDspAudioEngineFactory {
public:
    DspEngineCreateResult create(const DspEngineCreateRequest &request,
                                 const DspEngineContext &context) override;
    void destroy(DspEngineCreateResult &result) override;

private:
    DspEngineCreateResult nativeFallback(const DspEngineContext &context, const char *error);
    char errorText[192] {};
};
