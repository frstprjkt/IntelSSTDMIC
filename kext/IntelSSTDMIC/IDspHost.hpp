// Services a DSP engine needs from the IOService: logging, controller power and state publish.
#pragma once

class IDspHost {
public:
    virtual ~IDspHost() {}
    virtual void logLine(const char *fmt, ...) __attribute__((format(printf, 2, 3))) = 0;
    virtual bool holdPower() = 0;
    virtual void releasePower() = 0;
    virtual void stateChanged() = 0;
};
