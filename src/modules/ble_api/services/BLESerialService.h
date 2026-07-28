#pragma once
#if !defined(LITE_VERSION)
#include "BruceBLEService.hpp"
#include "ByteRing.h"

#include <SerialDevice.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#define BUFFER_SIZE 128
#define BLE_RX_RING_SIZE 512

class BLESerialCallbacks;
class BLEEventCallbacks;

// Unambiguous end-of-response marker, sent on the CLI characteristic after each
// command completes. The human-facing "# " prompt cannot serve as one: any
// output line that happens to begin with "# " — a dumped markdown file, a
// commented config, a script listing — would truncate the response and
// desynchronise every command after it. EOT is a control byte that cannot occur
// in the CLI's text output.
#define BLE_RESPONSE_EOT 0x04

class BLESerialService : public BruceBLEService, public SerialDevice {
    NimBLECharacteristic *serial_char = nullptr;
    NimBLECharacteristic *event_char = nullptr;
    BLESerialCallbacks *callbacks = nullptr;
    BLEEventCallbacks *event_callbacks = nullptr;
    // This NimBLE build exposes no subscriber count, so track it from the
    // CCCD callback. Written on the NimBLE host task, read from the loop task;
    // a stale read only costs one wasted notify, so no lock is warranted.
    volatile bool event_subscribed = false;
    // Set when a chunk's notify exhausted its retries, so endOfResponse() can
    // tell the client the reply is incomplete.
    volatile bool truncated = false;
    ByteRing<BLE_RX_RING_SIZE> rx;
    // Guards rx: pushRx() runs on the NimBLE host task, available()/read()/
    // readStringUntil() are polled from the Arduino loop task.
    SemaphoreHandle_t rxMutex = nullptr;
    void notifyChunkedTo(NimBLECharacteristic *chr, const uint8_t *data, size_t len);
    void notifyChunked(const uint8_t *data, size_t len);

public:
    BLESerialService();
    ~BLESerialService() override;
    void setup(NimBLEServer *pServer) override;
    void end() override;
    size_t println() override;
    size_t println(size_t n) override;
    size_t println(const String &s) override;
    size_t println(int n, int format) override;
    size_t print(const String &s) override;
    size_t print(int n, int format = DEC) override;
    void vprintf(const char *str, va_list args) override;
    size_t println(uint32_t n) override;
    size_t write(uint8_t *str, size_t size) override;
    int read() override;
    void flush() override {}
    String readStringUntil(char terminator) override;
    int available() override;
    bool hasLine(char terminator) override;
    void setMTU(uint16_t mtu);
    // Called from the characteristic write callback.
    void pushRx(const uint8_t *data, size_t len);

    // Emits BLE_RESPONSE_EOT so the client knows a command's output is complete.
    void endOfResponse() override;

    // Event frames go out on their own notify-only characteristic, so async
    // {"id":..} JSON never interleaves with CLI stdout on one byte stream.
    void notifyEvent(const String &frame);
    bool hasEventSubscriber() const;
    // Called from the event characteristic's CCCD callback.
    void setEventSubscribed(bool subscribed);
};
#endif
