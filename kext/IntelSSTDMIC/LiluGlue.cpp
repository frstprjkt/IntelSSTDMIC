// Lilu integration.
//
// Registration is event-driven: the AppleHDA KextInfo listener is registered at plugin start and
// the route is installed when Lilu reports that AppleHDA has loaded (KextInfo::Loaded also covers
// an already-loaded AppleHDA). No route is attempted before that.
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <mach/kmod.h>
#include "BootArgs.hpp"
#include "HookManager.hpp"

static const char *bootargOff[] {
    "-sstdmicoff"
};

static const char *bootargDebug[] {
    "-sstdmicdbg"
};

static const char *bootargBeta[] {
    "-sstdmicbeta"
};

static const char *appleHDAPaths[] {
    "/System/Library/Extensions/AppleHDA.kext/Contents/MacOS/AppleHDA"
};

static KernelPatcher::KextInfo appleHDAInfo {
    "com.apple.driver.AppleHDA", appleHDAPaths, arrsize(appleHDAPaths), {true}, {}, KernelPatcher::KextInfo::Unloaded
};

static void appleHDALoaded(void *, KernelPatcher &patcher, size_t id, mach_vm_address_t address, size_t size) {
    HookManager::status().kextCb++;
    HookManager::install(patcher, id, address, size);
}

static void pluginStart() {
    HookManager::status().started = 1;
    bool enabled = BootArgs::present("sstdmiclayout");
    if (!enabled) {
        HookManager::status().arg = 0;
        IOLog("IntelSSTDMIC: Lilu hooks disabled (boot without -sstdmiclayout)\n");
        return;
    }
    HookManager::status().arg = 1;
    LiluAPI::Error kerr = lilu.onKextLoad(&appleHDAInfo, 1, appleHDALoaded, nullptr);
    HookManager::status().onKextErr = static_cast<uint32_t>(kerr) + 1;
    IOLog("IntelSSTDMIC: Lilu onKextLoad(AppleHDA) registration: %d\n", static_cast<int>(kerr));
}

// Custom kmod entry (LILU_CUSTOM_KMOD_INIT): the standard plugin_start kern_start is not compiled,
// so the Lilu handshake is done here with diagnostics for every stage.
extern "C" kern_return_t IntelSSTDMIC_kern_start(kmod_info_t *, void *) {
    HookManager::status().kmodEntry = 1;
    LiluAPI::Error err = lilu.requestAccess();
    HookManager::status().reqAccess = static_cast<uint32_t>(err) + 1;
    if (err == LiluAPI::Error::NoError) {
        bool debug = false;
        err = lilu.shouldLoad(xStringify(PRODUCT_NAME),
                              parseModuleVersion(xStringify(MODULE_VERSION)),
                              LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
                              bootargOff, arrsize(bootargOff),
                              bootargDebug, arrsize(bootargDebug),
                              bootargBeta, arrsize(bootargBeta),
                              KernelVersion::Catalina, KernelVersion::Tahoe, debug);
        HookManager::status().shouldLoad = static_cast<uint32_t>(err) + 1;
        if (err == LiluAPI::Error::NoError) pluginStart();
        lilu.releaseAccess();
    }
    return KERN_SUCCESS;
}

extern "C" kern_return_t IntelSSTDMIC_kern_stop(kmod_info_t *, void *) {
    return KERN_FAILURE;
}

PluginConfiguration ADDPR(config) {
    xStringify(PRODUCT_NAME),
    parseModuleVersion(xStringify(MODULE_VERSION)),
    LiluAPI::AllowNormal | LiluAPI::AllowInstallerRecovery | LiluAPI::AllowSafeMode,
    bootargOff,
    arrsize(bootargOff),
    bootargDebug,
    arrsize(bootargDebug),
    bootargBeta,
    arrsize(bootargBeta),
    KernelVersion::Catalina,
    KernelVersion::Tahoe,
    pluginStart
};
