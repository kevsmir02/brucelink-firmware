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

// subValue is the raw CCCD bitmask: 0 = unsubscribed, bit 0 = notify,
// bit 1 = indicate. Anything non-zero means somebody is listening.
class BLEEventCallbacks : public NimBLECharacteristicCallbacks {
    BLESerialService *owner;

    void onSubscribe(NimBLECharacteristic *, NimBLEConnInfo &, uint16_t subValue) override {
        owner->setEventSubscribed(subValue != 0);
    }

public:
    explicit BLEEventCallbacks(BLESerialService *owner) : owner(owner) {}
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

    // Notify-only: events flow device -> app exclusively. Mirrors the existing
    // /cm (request-response) + /ws (events) split on the HTTP side.
    event_char = pService->createCharacteristic(
        "d555ed98-bf2a-4f46-b3eb-d1fcdd7325e9", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    event_callbacks = new BLEEventCallbacks(this);
    event_char->setCallbacks(event_callbacks);

    pService->start();
    pServer->getAdvertising()->addServiceUUID(pService->getUUID());
}

void BLESerialService::end() {
    delete callbacks;
    callbacks = nullptr;
    delete event_callbacks;
    event_callbacks = nullptr;
    event_subscribed = false;
    // Borrowed pointers — the NimBLE service owns them and frees them on
    // deinit. Dropping them here stops notifyEvent()/endOfResponse() touching
    // freed memory between end() and the next setup().
    serial_char = nullptr;
    event_char = nullptr;
    if (rxMutex && xSemaphoreTake(rxMutex, pdMS_TO_TICKS(BLE_RX_MUTEX_TIMEOUT_MS)) == pdTRUE) {
        rx.clear();
        xSemaphoreGive(rxMutex);
    }
}

// Raw unread byte count. Deliberately NOT line-aware: the YMODEM transfer in
// core/serial_commands/storage_commands.cpp polls available() and read() for
// arbitrary binary bytes that contain no terminator. Line framing lives in
// hasLine() instead.
int BLESerialService::available() {
    if (!rxMutex) return 0;
    if (xSemaphoreTake(rxMutex, pdMS_TO_TICKS(BLE_RX_MUTEX_TIMEOUT_MS)) != pdTRUE) return 0;
    int n = (int)rx.size();
    xSemaphoreGive(rxMutex);
    return n;
}

// Measured on smoochiee-board: an unnegotiated MTU of 23 caps a characteristic
// write at 20 bytes, so any longer command genuinely arrives split across
// several writes. Reporting "ready" on a partial line would make
// handleSerialCommands() parse a fragment as though it were a whole command.
bool BLESerialService::hasLine(char terminator) {
    if (!rxMutex) return false;
    if (xSemaphoreTake(rxMutex, pdMS_TO_TICKS(BLE_RX_MUTEX_TIMEOUT_MS)) != pdTRUE) return false;
    bool ready = rx.contains((uint8_t)terminator);
    xSemaphoreGive(rxMutex);
    return ready;
}

// ATT payload is MTU minus the 3-byte notify header. Anything longer is dropped
// by the stack, so split it. Guard the floor: mtu defaults to 23 before
// negotiation, and a malformed negotiation could report less.
// No pacing delay between chunks: bleNotifyRetry() already applies backpressure,
// returning as soon as the notify is queued and yielding a tick only when the
// host's queue is full. A blanket sleep on the happy path was pure dead time —
// at the default MTU of 23 a 469-byte systeminfo reply spent 240ms asleep.
//
// A failed chunk aborts the rest. The return value used to be discarded, so a
// notify that exhausted its retries silently truncated the payload and the
// client saw a malformed reply with no indication why.
void BLESerialService::notifyChunkedTo(NimBLECharacteristic *chr, const uint8_t *data, size_t len) {
    if (!chr) return;
    size_t chunk = (mtu > 3) ? (size_t)(mtu - 3) : 20;
    for (size_t off = 0; off < len; off += chunk) {
        size_t n = (len - off < chunk) ? (len - off) : chunk;
        if (!bleNotifyRetry(chr, data + off, n)) {
            Serial.printf(
                "BLESerialService: notify failed at offset %u of %u, payload truncated\n",
                (unsigned)off,
                (unsigned)len
            );
            return;
        }
    }
}

void BLESerialService::notifyChunked(const uint8_t *data, size_t len) {
    notifyChunkedTo(serial_char, data, len);
}

void BLESerialService::endOfResponse() {
    const uint8_t eot = BLE_RESPONSE_EOT;
    notifyChunkedTo(serial_char, &eot, 1);
}

void BLESerialService::setEventSubscribed(bool subscribed) { event_subscribed = subscribed; }

bool BLESerialService::hasEventSubscriber() const { return event_char != nullptr && event_subscribed; }

// Frames are newline-delimited so a client can split a chunked stream back into
// whole JSON objects. Dropped when nobody is subscribed rather than queued —
// the app resumes via lastEventId replay, so buffering here would only add a
// second, weaker recovery path.
void BLESerialService::notifyEvent(const String &frame) {
    if (!hasEventSubscriber()) return;
    String line = frame + "\n";
    notifyChunkedTo(event_char, reinterpret_cast<const uint8_t *>(line.c_str()), line.length());
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

    // With no terminator buffered there is no complete line to return. Leave the
    // bytes in the ring for the next call rather than draining them: the rest of
    // the command is still arriving in a later characteristic write, and
    // returning the fragment would hand a truncated command to the parser.
    //
    // This previously assumed one whole command per write, which held only while
    // the MTU was large enough to fit it. It is not: writing "system" then
    // "info\n" as two writes produced `ERROR: Command not found at 'system'`
    // followed by the output of `info`, instead of running `systeminfo`.
    if (!rx.contains((uint8_t)terminator)) {
        xSemaphoreGive(rxMutex);
        return result;
    }

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
