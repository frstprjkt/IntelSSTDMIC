#include "HdaAudioEngine.hpp"
#include "Regs.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>

namespace {
constexpr uint32_t PCI_PGCTL = 0x44, PCI_PGCTL_ADSPPGD = 1u << 2;
constexpr uint32_t PCI_CGCTL = 0x48, PCI_CGCTL_ADSPDCGE = 1u << 1;
constexpr uint32_t PCI_DEVC  = 0x78; constexpr uint16_t PCI_DEVC_NSNPEN = 1u << 11;
constexpr uint32_t HDA_EM2 = 0x1030, EM2_L1SEN = 1u << 13;
constexpr uint32_t SD_STS = 0x03, SD_STS_MASK = 0x1C;
constexpr uint32_t SD_CTL_INT_MASK = 0x1C;
}

HdaAudioEngine::~HdaAudioEngine() {
    detach();
}

// ---- default native behaviour -------------------------------------------------------------

bool HdaAudioEngine::initHardware(IOService *self, IOService *provider) {
    return nativeOps && nativeOps->initHardware ? nativeOps->initHardware(self, provider) : false;
}

IOReturn HdaAudioEngine::performAudioEngineStart(IOService *self) {
    return nativeOps && nativeOps->performAudioEngineStart
        ? nativeOps->performAudioEngineStart(self) : kIOReturnUnsupported;
}

IOReturn HdaAudioEngine::performAudioEngineStop(IOService *self) {
    return nativeOps && nativeOps->performAudioEngineStop
        ? nativeOps->performAudioEngineStop(self) : kIOReturnUnsupported;
}

void HdaAudioEngine::stopHardware(IOService *self, IOService *provider) {
    if (nativeOps && nativeOps->stopHardware) nativeOps->stopHardware(self, provider);
}

IOReturn HdaAudioEngine::performFormatChange(IOService *self, IOAudioStream *stream,
                                             const IOAudioStreamFormat *format,
                                             const IOAudioSampleRate *rate) {
    return nativeOps && nativeOps->performFormatChange
        ? nativeOps->performFormatChange(self, stream, format, rate) : kIOReturnUnsupported;
}

IOReturn HdaAudioEngine::convertInputSamples(IOService *self, const void *sampleBuf, void *destBuf,
                                             UInt32 firstFrame, UInt32 numFrames,
                                             const IOAudioStreamFormat *format,
                                             IOAudioStream *stream) {
    return nativeOps && nativeOps->convertInputSamples
        ? nativeOps->convertInputSamples(self, sampleBuf, destBuf, firstFrame, numFrames, format, stream)
        : kIOReturnUnsupported;
}

// ---- hardware access ----------------------------------------------------------------------

bool HdaAudioEngine::attach(IOPCIDevice *pci) {
    if (!pci) return false;
    if (!pciDev) {
        pciDev = pci;
        pciDev->retain();
    }
    if (!bar0Map) {
        bar0Map = pciDev->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
        if (!bar0Map) return false;
        hdaRegs = reinterpret_cast<volatile uint8_t *>(bar0Map->getVirtualAddress());
    }
    return hdaRegs != nullptr;
}

void HdaAudioEngine::detach() {
    OSSafeReleaseNULL(bar4Map);
    OSSafeReleaseNULL(bar0Map);
    hdaRegs = nullptr;
    dspRegs = nullptr;
    OSSafeReleaseNULL(pciDev);
    controllerReady = false;
}

bool HdaAudioEngine::ensureDspMapped() {
    if (dspRegs) return true;
    if (!pciDev) return false;
    OSSafeReleaseNULL(bar4Map);
    bar4Map = pciDev->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress4);
    if (!bar4Map) return false;
    dspRegs = reinterpret_cast<volatile uint8_t *>(bar4Map->getVirtualAddress());
    return dspRegs != nullptr;
}

bool HdaAudioEngine::memoryDecoded() const {
    return pciDev && (pciDev->configRead16(kIOPCIConfigCommand) & kIOPCICommandMemorySpace);
}

bool HdaAudioEngine::inD0() const {
    if (!pciDev) return false;
    uint8_t pm = 0;
    if (!pciDev->findPCICapability(kIOPCIPowerManagementCapability, &pm)) return true;
    return (pciDev->configRead16(pm + 4) & 0x3) == 0;
}

bool HdaAudioEngine::waitUsable(uint32_t timeoutUs) {
    for (uint32_t t = 0; t <= timeoutUs; t += 1000) {
        if (memoryDecoded() && inD0() && hdaRegs) {
            uint32_t g = r32(HDA::GCTL);
            if (g != 0xFFFFFFFF && (g & 1)) return true;
        }
        IOSleep(1);
    }
    return false;
}

bool HdaAudioEngine::probe() {
    if (!hdaRegs) return false;
    if (!memoryDecoded() || !inD0()) return false;
    if (!(r32(HDA::GCTL) & 1)) return false;
    if (controllerReady) return true;
    uint16_t gcap = r16(HDA::GCAP);
    if (gcap == 0xFFFF || gcap == 0) return false;
    if (!(r32(HDA::LLCH) & HDA::CAP_NEXT_MASK)) return false;
    numInputStreams = (gcap >> 8) & 0xF;
    numOutputStreams = (gcap >> 12) & 0xF;
    numBidirStreams = (gcap >> 3) & 0x1F;
    if (!walkCapabilities()) {
        ppCapOffset = mlCapOffset = spbCapOffset = drsmCapOffset = 0;
        return false;
    }
    controllerReady = true;
    return true;
}

bool HdaAudioEngine::walkCapabilities() {
    uint32_t off = r32(HDA::LLCH) & HDA::CAP_NEXT_MASK;
    for (int n = 0; off && off < bar0Map->getLength() && n < 10; n++) {
        uint32_t hdr = r32(off);
        uint32_t id = (hdr & HDA::CAP_ID_MASK) >> HDA::CAP_ID_SHIFT;
        switch (id) {
            case HDA::CAP_PP:   ppCapOffset = off; break;
            case HDA::CAP_ML:   mlCapOffset = off; break;
            case HDA::CAP_SPB:  spbCapOffset = off; break;
            case HDA::CAP_DRSM: drsmCapOffset = off; break;
        }
        off = hdr & HDA::CAP_NEXT_MASK;
    }
    return ppCapOffset != 0;
}

uint32_t HdaAudioEngine::r32(uint32_t off) const {
    return hdaRegs ? *reinterpret_cast<volatile uint32_t *>(hdaRegs + off) : 0xFFFFFFFF;
}
uint16_t HdaAudioEngine::r16(uint32_t off) const {
    return hdaRegs ? *reinterpret_cast<volatile uint16_t *>(hdaRegs + off) : 0xFFFF;
}
uint8_t HdaAudioEngine::r8(uint32_t off) const {
    return hdaRegs ? hdaRegs[off] : 0xFF;
}
void HdaAudioEngine::w32(uint32_t off, uint32_t v) const {
    if (hdaRegs) *reinterpret_cast<volatile uint32_t *>(hdaRegs + off) = v;
}
void HdaAudioEngine::w16(uint32_t off, uint16_t v) const {
    if (hdaRegs) *reinterpret_cast<volatile uint16_t *>(hdaRegs + off) = v;
}
void HdaAudioEngine::w8(uint32_t off, uint8_t v) const {
    if (hdaRegs) hdaRegs[off] = v;
}
uint32_t HdaAudioEngine::d32(uint32_t off) const {
    return dspRegs ? *reinterpret_cast<volatile uint32_t *>(dspRegs + off) : 0xFFFFFFFF;
}
void HdaAudioEngine::dw32(uint32_t off, uint32_t v) const {
    if (dspRegs) *reinterpret_cast<volatile uint32_t *>(dspRegs + off) = v;
}

bool HdaAudioEngine::pollHda(uint32_t off, uint32_t mask, uint32_t want,
                             uint32_t timeoutUs, uint32_t *last) {
    uint32_t v = 0;
    for (uint32_t t = 0; t <= timeoutUs; t += 10) {
        v = r32(off);
        if (v == 0xFFFFFFFF) break;
        if ((v & mask) == want) { if (last) *last = v; return true; }
        IODelay(10);
    }
    if (last) *last = v;
    return false;
}

bool HdaAudioEngine::pollDsp(uint32_t off, uint32_t mask, uint32_t want,
                             uint32_t timeoutUs, uint32_t *last) {
    uint32_t v = 0;
    for (uint32_t t = 0; t <= timeoutUs; t += 100) {
        v = d32(off);
        if (v == 0xFFFFFFFF) break;
        if ((v & mask) == want) { if (last) *last = v; return true; }
        IODelay(100);
    }
    if (last) *last = v;
    return false;
}

bool HdaAudioEngine::streamStopReset(uint32_t sd) {
    w32(sd, r32(sd) & ~(HDA::SD_CTL_RUN | SD_CTL_INT_MASK));
    if (!pollHda(sd, HDA::SD_CTL_RUN, 0, 300) && !pollHda(sd, HDA::SD_CTL_RUN, 0, 20000))
        return false;
    w8(sd + SD_STS, SD_STS_MASK);
    w32(sd, r32(sd) | HDA::SD_CTL_SRST);
    if (!pollHda(sd, HDA::SD_CTL_SRST, HDA::SD_CTL_SRST, 300)) return false;
    w32(sd, r32(sd) & ~HDA::SD_CTL_SRST);
    return pollHda(sd, HDA::SD_CTL_SRST, 0, 300);
}

void HdaAudioEngine::gating(bool enable) {
    if (!pciDev || !hdaRegs) return;
    if (!enable) {
        if (gatingDisabled) return;
        gatingDisabled = true;
        savedCgctl = pciDev->configRead32(PCI_CGCTL);
        savedPgctl = pciDev->configRead32(PCI_PGCTL);
        savedEm2 = r32(HDA_EM2);
        pciDev->configWrite32(PCI_CGCTL, savedCgctl & ~PCI_CGCTL_ADSPDCGE);
        w32(HDA_EM2, savedEm2 & ~EM2_L1SEN);
        pciDev->configWrite32(PCI_PGCTL, savedPgctl | PCI_PGCTL_ADSPPGD);
    } else {
        if (!gatingDisabled) return;
        gatingDisabled = false;
        pciDev->configWrite32(PCI_PGCTL, savedPgctl);
        w32(HDA_EM2, savedEm2);
        pciDev->configWrite32(PCI_CGCTL, savedCgctl);
    }
}

void HdaAudioEngine::setNoSnoop(bool on) {
    if (!pciDev) return;
    int mode = 1;
    PE_parse_boot_argn("sstdmicsnoop", &mode, sizeof(mode));
    if (mode == 0) return;
    if (on) {
        savedDevc = pciDev->configRead16(PCI_DEVC);
        devcChanged = !(savedDevc & PCI_DEVC_NSNPEN);
        if (devcChanged) pciDev->configWrite16(PCI_DEVC, savedDevc | PCI_DEVC_NSNPEN);
    } else if (devcChanged && mode != 2) {
        uint16_t cur = pciDev->configRead16(PCI_DEVC);
        pciDev->configWrite16(PCI_DEVC, (cur & ~PCI_DEVC_NSNPEN) | (savedDevc & PCI_DEVC_NSNPEN));
        devcChanged = false;
    }
}
