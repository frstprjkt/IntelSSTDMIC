// IntelSSTDMIC service: layout selection, manager/factory ownership, HDA power management and
// ioreg diagnostics. Hook handler classes live behind DspAudioEngineManager.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include "IDspHost.hpp"
#include "LayoutConfig.hpp"
#include "DspAudioEngineFactory.hpp"

class IOPCIDevice;
class DspAudioEngineManager;
class OSArray;

#define kIntelSSTDMICVersion "1.0.0"

class IntelSSTDMIC : public IOService, public IDspHost {
    OSDeclareDefaultStructors(IntelSSTDMIC)
public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;
    IOReturn setPowerState(unsigned long state, IOService *device) override;

    static IntelSSTDMIC *instance() { return gInstance; }

    // IDspHost
    void logLine(const char *fmt, ...) override __attribute__((format(printf, 2, 3)));
    bool holdPower() override;
    void releasePower() override;
    void stateChanged() override { publish(); }

private:
    static IntelSSTDMIC *gInstance;

    IOPCIDevice *pci {nullptr};
    LayoutRegistry registry;
    const LayoutConfig *layout {nullptr};
    DspAudioEngineFactory factory;
    DspAudioEngineManager *manager {nullptr};

    PlatformFamily platformFamily {PlatformFamily::Unknown};
    uint32_t pciId {0};
    bool injectorEnabled {false};
    bool layoutDisabled {false};
    bool layoutRequested {false};
    volatile bool selfReleasingPower {false};

    IOLock *logLock {nullptr};
    OSArray *logRing {nullptr};

    bool pmJoined {false};
    IOService *pmParent {nullptr};
    uint32_t pmSearchTicks {0};
    unsigned long pmState {0};

    void publish();
    void detectPlatform(uint16_t device);
    const LayoutConfig *selectLayout();
    void joinPowerParent();
    bool memoryDecoded() const;
    bool inD0() const;
};
