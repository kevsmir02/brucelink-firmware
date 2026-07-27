#ifndef BRUCE_SERIALDEVICE_H
#define BRUCE_SERIALDEVICE_H

#include <Arduino.h>

class SerialDevice {
public:
    virtual size_t println(const String &s) = 0;
    virtual size_t println(size_t n) = 0;
    virtual size_t println(uint32_t n) = 0;
    virtual size_t println() = 0;
    virtual size_t println(int n, int format = DEC) = 0;
    virtual size_t print(int n, int format = DEC) = 0;
    virtual size_t print(const String &s) = 0;
    virtual void vprintf(const char *fmt, va_list args) = 0;
    virtual int read() = 0;
    virtual size_t write(uint8_t *str, size_t size) = 0;
    void printf(const char *fmt, ...) {
        va_list args;
        va_start(args, fmt);
        vprintf(fmt, args);
        va_end(args);
    }
    virtual void flush() = 0;

    virtual int available() = 0;
    virtual String readStringUntil(char terminator) = 0;

    // True when a complete `terminator`-delimited line is buffered, so
    // readStringUntil() will return a whole one rather than a fragment.
    //
    // Distinct from available() on purpose: available() is a raw byte count and
    // binary consumers depend on it (the YMODEM transfer in
    // core/serial_commands/storage_commands.cpp reads arbitrary bytes that never
    // contain a terminator). Line-framed callers must ask this instead.
    //
    // Stream-backed transports block internally until the terminator or a
    // timeout, so "has any bytes" is the right answer for them.
    virtual bool hasLine(char terminator) {
        (void)terminator;
        return available() > 0;
    }
    virtual ~SerialDevice() = default;
};

#endif // BRUCE_SERIALDEVICE_H
