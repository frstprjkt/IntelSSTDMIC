#include <mach/mach_types.h>

// libkmod's _start/_stop wrappers forward through _realmain/_antimain. Lilu's plugin_start.cpp
// provides these entry points; IntelSSTDMIC_kern_* performs the Lilu handshake and registers the
// AppleHDA load listener.
extern kern_return_t _start(kmod_info_t *ki, void *data);
extern kern_return_t _stop(kmod_info_t *ki, void *data);
extern kern_return_t IntelSSTDMIC_kern_start(kmod_info_t *ki, void *data);
extern kern_return_t IntelSSTDMIC_kern_stop(kmod_info_t *ki, void *data);

__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(com.labrouss.IntelSSTDMIC, "1.0.4", _start, _stop)
__private_extern__ kmod_start_func_t *_realmain = IntelSSTDMIC_kern_start;
__private_extern__ kmod_stop_func_t *_antimain = IntelSSTDMIC_kern_stop;
__private_extern__ int _kext_apple_cc = __APPLE_CC__;
