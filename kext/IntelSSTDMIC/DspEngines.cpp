#include "IntelDspEngines.hpp"
#include <pexpert/pexpert.h>

extern "C" const unsigned char kSofTglRi[];
extern "C" const unsigned int kSofTglRiSize;
extern "C" const unsigned char kSofCmlRi[];
extern "C" const unsigned int kSofCmlRiSize;
extern "C" const unsigned char kSofAdlRi[];
extern "C" const unsigned int kSofAdlRiSize;

uint32_t coreMaskOverride(uint32_t def) {
    int maskArg = -1;
    if (PE_parse_boot_argn("sstdmiccoremask", &maskArg, sizeof(maskArg)) && maskArg >= 1)
        return (uint32_t)maskArg;
    return def;
}

const uint8_t *TglDspAudioEngine::firmwareImage(uint32_t &size) const {
    size = kSofTglRiSize;
    return kSofTglRi;
}

uint32_t TglDspAudioEngine::targetCoreMask() const {
    return coreMaskOverride(1u);
}

const uint8_t *AdlDspAudioEngine::firmwareImage(uint32_t &size) const {
    size = kSofAdlRiSize;
    return kSofAdlRi;
}

uint32_t AdlDspAudioEngine::targetCoreMask() const {
    return coreMaskOverride(1u);
}

const uint8_t *CmlDspAudioEngine::firmwareImage(uint32_t &size) const {
    size = kSofCmlRiSize;
    return kSofCmlRi;
}

uint32_t CmlDspAudioEngine::targetCoreMask() const {
    return coreMaskOverride(0x0Fu);
}
