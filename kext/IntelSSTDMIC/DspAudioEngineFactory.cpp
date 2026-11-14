#include "DspAudioEngineFactory.hpp"
#include "IntelDspEngines.hpp"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

DspEngineCreateResult DspAudioEngineFactory::nativeFallback(const DspEngineContext &context,
                                                            const char *error) {
    DspEngineCreateResult result {};
    if (!error)
        errorText[0] = 0;
    else if (error != errorText)
        snprintf(errorText, sizeof(errorText), "%s", error);
    HdaAudioEngine *native = new HdaAudioEngine();
    if (!native) {
        char detail[192];
        snprintf(detail, sizeof(detail), "%s", error ? error : "unknown");
        snprintf(errorText, sizeof(errorText), "%s (native allocation failed)", detail);
        result.error = errorText;
        return result;
    }
    native->setNativeOps(&context.ops);
    result.native = native;
    result.hooks = native;
    result.error = errorText;
    return result;
}

DspEngineCreateResult DspAudioEngineFactory::create(const DspEngineCreateRequest &request,
                                                    const DspEngineContext &context) {
    DspEngineCreateResult result {};
    if (request.source == DspEngineCreateSource::Disabled) {
        result = nativeFallback(context, nullptr);
        result.disabled = true;
        result.error = nullptr;
        return result;
    }
    const LayoutConfig *layout = request.layout;
    if (!layout || !layout->valid)
        return nativeFallback(context, "layout not found or invalid");

    PlatformFamily platform = layout->platform != PlatformFamily::Unknown
        ? layout->platform : request.platform;
    const char *engine = layout->engine;

    IntelDspAudioEngine *active = nullptr;
    if (!strcmp(engine, "adl") || platform == PlatformFamily::AlderLake)
        active = new AdlDspAudioEngine();
    else if (!strcmp(engine, "tgl") || platform == PlatformFamily::TigerLake)
        active = new TglDspAudioEngine();
    else if (!strcmp(engine, "cml") || platform == PlatformFamily::CometLake)
        active = new CmlDspAudioEngine();

    if (!active) {
        snprintf(errorText, sizeof(errorText), "layout %u: unsupported engine '%s'", layout->id, engine);
        return nativeFallback(context, errorText);
    }
    if (!active->initialize(context)) {
        snprintf(errorText, sizeof(errorText), "layout %u initialize failed: %s",
                 layout->id, active->lastError());
        delete active;
        return nativeFallback(context, errorText);
    }

    result.engine = active;
    result.hooks = active->hooks();
    result.error = nullptr;
    IOLog("IntelSSTDMIC: created %s from layout %u (%s)\n",
          active->engineName(), layout->id, layout->name);
    return result;
}

void DspAudioEngineFactory::destroy(DspEngineCreateResult &result) {
    if (result.engine) {
        result.engine->shutdown();
        delete result.engine;
    }
    if (result.native) delete result.native;
    result = DspEngineCreateResult {};
}
