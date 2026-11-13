#include "LayoutConfig.hpp"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>

// Generated at build time from Layouts/Layouts.plist (also copied into the kext Info.plist).
extern "C" void sstdmEmbeddedLayouts(LayoutRegistry &registry);

namespace {
bool contains(const char *hay, const char *needle) {
    if (!hay || !needle || !*needle) return false;
    size_t n = strlen(needle);
    for (const char *p = hay; *p; p++)
        if (!strncmp(p, needle, n)) return true;
    return false;
}
}

const LayoutConfig &LayoutRegistry::none() {
    static const LayoutConfig invalid {};
    return invalid;
}

LayoutRegistry::LayoutRegistry() {
    sstdmEmbeddedLayouts(*this);
    IOLog("IntelSSTDMIC: loaded %u embedded layout(s)\n", layoutCount);
}

bool LayoutRegistry::add(const LayoutConfig &layout) {
    if (!layout.valid) return false;
    for (uint32_t i = 0; i < layoutCount; i++)
        if (layouts[i].id == layout.id) { layouts[i] = layout; return true; }
    if (layoutCount >= kMaxLayouts) return false;
    layouts[layoutCount++] = layout;
    return true;
}

const LayoutConfig *LayoutRegistry::find(uint32_t id) const {
    for (uint32_t i = 0; i < layoutCount; i++)
        if (layouts[i].id == id) return &layouts[i];
    return nullptr;
}

const LayoutConfig *LayoutRegistry::findByName(const char *name) const {
    if (!name || !name[0]) return nullptr;
    for (uint32_t i = 0; i < layoutCount; i++) {
        if (contains(layouts[i].name, name)) return &layouts[i];
        char idbuf[16];
        snprintf(idbuf, sizeof(idbuf), "%u", layouts[i].id);
        if (contains(idbuf, name)) return &layouts[i];
    }
    return nullptr;
}

const LayoutConfig *LayoutRegistry::defaultFor(PlatformFamily platform) const {
    for (uint32_t i = 0; i < layoutCount; i++)
        if (layouts[i].platform == platform) return &layouts[i];
    return &none();
}

const LayoutConfig *LayoutRegistry::at(uint32_t index) const {
    return index < layoutCount ? &layouts[index] : nullptr;
}

const LayoutConfig *LayoutRegistry::resolve(int32_t layoutId, const char *layoutName,
                                            const char *model, PlatformFamily platform) const {
    if (layoutId >= 0) {
        if (const LayoutConfig *p = find((uint32_t)layoutId)) return p;
        return &none();
    }
    if (layoutName && layoutName[0]) {
        if (const LayoutConfig *p = findByName(layoutName)) return p;
        return &none();
    }
    if (model && model[0]) {
        if (const LayoutConfig *p = findByName(model)) return p;
        return &none();
    }
    return defaultFor(platform);
}
