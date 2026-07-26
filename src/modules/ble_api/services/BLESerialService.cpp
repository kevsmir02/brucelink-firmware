#if !defined(LITE_VERSION)
#include "BLESerialService.h"
#include "modules/ble/ble_common.h" // bleNotifyRetry
#include <NimBLEDevice.h>

BLESerialService::BLESerialService() : BruceBLEService() {}

BLESerialService::~BLESerialService() {}

class BLESerialCallbacks : public NimBLECharacteristicCallbacks {
    BLESerialService *owner;

    void onWrite(NimBLECharacteristic *pCharacteristic, NimBLEConnInfo &connInfo) override {
        std::string v = pCharacteristic->getValue();
        if (!v.empty()) owner->pushRx(reinterpret_cast<const uint8_t *>(v.data()), v.size());
    }

public:
    explicit BLESerialCallbacks(BLESerialService *owner) : owner(owner) {}
};

void BLESerialService::pushRx(const uint8_t *data, size_t len) { rx.write(data, len); }

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
    rx.clear();
}

int BLESerialService::available() { return (int)rx.size(); }

size_t BLESerialService::println(const String &s) {
    String toSend = s + "\r\n";
    bleNotifyRetry(serial_char, reinterpret_cast<const uint8_t *>(toSend.c_str()), toSend.length());
    vTaskDelay(pdMS_TO_TICKS(10)); // Add some delay to ensure data is read by the client
    return toSend.length();
}

size_t BLESerialService::print(const String &s) {
    bleNotifyRetry(serial_char, reinterpret_cast<const uint8_t *>(s.c_str()), s.length());
    vTaskDelay(pdMS_TO_TICKS(10));
    return s.length();
}

size_t BLESerialService::println(size_t n) {
    String s = String(n);
    return println(s);
}

void BLESerialService::vprintf(const char *fmt, va_list args) {
    int size = vsnprintf(NULL, 0, fmt, args) + 1;
    char str[BUFFER_SIZE];
    sprintf(str, fmt, args);

    bleNotifyRetry(serial_char, reinterpret_cast<const uint8_t *>(str), size);
    vTaskDelay(pdMS_TO_TICKS(10));
}

String BLESerialService::readStringUntil(char terminator) {
    String result = "";
    int c;
    while ((c = rx.read()) >= 0) {
        if ((char)c == terminator) break;
        result += (char)c;
    }
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
    bleNotifyRetry(serial_char, str, size);
    vTaskDelay(pdMS_TO_TICKS(10));
    return size;
}

int BLESerialService::read() { return rx.read(); }

void BLESerialService::setMTU(uint16_t mtu) { this->mtu = mtu; }

#endif
