#if !defined(LITE_VERSION)
#include "BLESerialService.h"
#include "modules/ble/ble_common.h" // bleNotifyRetry
#include <NimBLEDevice.h>

// Bounded wait for rxMutex. pushRx() (NimBLE host task) and
// available()/read()/readStringUntil() (Arduino loop task, via
// handleSerialCommands) run on different FreeRTOS tasks and touch ByteRing's
// non-atomic head/tail/count fields, so this is genuine cross-task access,
// not just interleaving. A short bounded timeout means neither task ever
// blocks indefinitely; on timeout, callers degrade to "no data" rather than
// risk corrupting the ring.
#define BLE_RX_MUTEX_TIMEOUT_MS 50

BLESerialService::BLESerialService() : BruceBLEService() { rxMutex = xSemaphoreCreateMutex(); }

BLESerialService::~BLESerialService() {
    if (rxMutex) vSemaphoreDelete(rxMutex);
}

class BLESerialCallbacks : public NimBLECharacteristicCallbacks {
    BLESerialService *owner;

    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
        std::string v = pCharacteristic->getValue();
        if (!v.empty()) owner->pushRx(reinterpret_cast<const uint8_t *>(v.data()), v.size());
    }

public:
    explicit BLESerialCallbacks(BLESerialService *owner) : owner(owner) {}
};

void BLESerialService::pushRx(const uint8_t *data, size_t len) {
    if (!rxMutex) return;
    if (xSemaphoreTake(rxMutex, pdMS_TO_TICKS(BLE_RX_MUTEX_TIMEOUT_MS)) != pdTRUE) return;
    size_t accepted = rx.write(data, len);
    xSemaphoreGive(rxMutex);

    // Should never fire: BLE_RX_RING_SIZE (512) comfortably holds several
    // queued sub-MTU commands. If it does, an overflow is silently truncating
    // a command, so surface it loudly rather than let it fail mysteriously.
    if (accepted < len) {
        Serial.printf(
            "BLESerialService: RX ring overflow, dropped %u of %u bytes\n",
            (unsigned)(len - accepted),
            (unsigned)len
        );
    }
}

void BLESerialService::setup(NimBLEServer *pServer) {
    pService = pServer->createService("4371ec0b-3d43-49f9-b731-7c72a4a7bb91");

    serial_char = pService->createCharacteristic(
        "d555ed97-bf2a-4f46-b3eb-d1fcdd7325e9", // Battery Level
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | NIMBLE_PROPERTY::WRITE
    );

    callbacks = new BLESerialCallbacks(this);
    serial_char->setCallbacks(callbacks);

    pService->start();
    pServer->getAdvertising()->addServiceUUID(pService->getUUID());
}

void BLESerialService::end() {
    delete callbacks;
    callbacks = nullptr;
    if (rxMutex && xSemaphoreTake(rxMutex, pdMS_TO_TICKS(BLE_RX_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        rx.clear();
        xSemaphoreGive(rxMutex);
    }
}

int BLESerialService::available() {
    if (!rxMutex) return 0;
    if (xSemaphoreTake(rxMutex, pdMS_TO_TICKS(BLE_RX_MUTEX_TIMEOUT_MS)) != pdTRUE) return 0;
    int n = (int)rx.size();
    xSemaphoreGive(rxMutex);
    return n;
}

// ATT payload is MTU minus the 3-byte notify header. Anything longer is dropped
// by the stack, so split it. Guard the floor: mtu defaults to 23 before
// negotiation, and a malformed negotiation could report less.
void BLESerialService::notifyChunked(const uint8_t *data, size_t len) {
    size_t chunk = (mtu > 3) ? (size_t)(mtu - 3) : 20;
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = (len - off < chunk) ? (len - off) : chunk;
        bleNotifyRetry(serial_char, data + off, n);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

size_t BLESerialService::println(const String &s) {
    String toSend = s + "\r\n";
    notifyChunked(reinterpret_cast<const uint8_t *>(toSend.c_str()), toSend.length());
    return toSend.length();
}

size_t BLESerialService::print(const String &s) {
    notifyChunked(reinterpret_cast<const uint8_t *>(s.c_str()), s.length());
    return s.length();
}

size_t BLESerialService::println(size_t n) {
    String s = String(n);
    return println(s);
}

void BLESerialService::vprintf(const char *fmt, va_list args) {
    char str[BUFFER_SIZE];
    int n = vsnprintf(str, sizeof(str), fmt, args);
    if (n < 0) return;
    size_t len = ((size_t)n < sizeof(str)) ? (size_t)n : sizeof(str) - 1;
    notifyChunked(reinterpret_cast<const uint8_t *>(str), len);
}

String BLESerialService::readStringUntil(char terminator) {
    String result = "";
    if (!rxMutex) return result;
    // Held across the whole drain loop, not per-read(), so a concurrent
    // pushRx() on the NimBLE host task can't interleave bytes mid-line. The
    // loop is bounded by the ring size, so holding the lock this long is
    // safe.
    if (xSemaphoreTake(rxMutex, pdMS_TO_TICKS(BLE_RX_MUTEX_TIMEOUT_MS)) != pdTRUE) return result;

    // NOTE: if no terminator byte is present, this drains the whole ring and
    // returns the fragment, indistinguishable from a complete line. That's
    // safe only because of the write-side invariant this transport relies
    // on: the app always sends one complete, `\n`-terminated command per
    // characteristic write, well under the negotiated MTU.
    int c;
    while ((c = rx.read()) >= 0) {
        if ((char)c == terminator) break;
        result += (char)c;
    }
    xSemaphoreGive(rxMutex);
    return result;
}

size_t BLESerialService::println(const uint32_t n) {
    String s = String(n);
    return println(s);
}

size_t BLESerialService::print(const int n, int format) {
    String s = String(n, format);
    return print(s);
}

size_t BLESerialService::println(const int n, int format) {
    String s = String(n, format);
    return println(s);
}

size_t BLESerialService::println() { return println(""); }

size_t BLESerialService::write(uint8_t *str, size_t size) {
    notifyChunked(str, size);
    return size;
}

int BLESerialService::read() {
    if (!rxMutex) return -1;
    if (xSemaphoreTake(rxMutex, pdMS_TO_TICKS(BLE_RX_MUTEX_TIMEOUT_MS)) != pdTRUE) return -1;
    int c = rx.read();
    xSemaphoreGive(rxMutex);
    return c;
}

void BLESerialService::setMTU(uint16_t mtu) { this->mtu = mtu; }

#endif
