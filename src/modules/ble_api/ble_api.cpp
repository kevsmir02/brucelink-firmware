#if !defined(LITE_VERSION)
#include "ble_api.hpp"
#include <NimBLEDevice.h>
#include <core/USBSerial/USBSerial.h>
#include <core/wifi/ws_events.h>
#include <globals.h>

BLE_API::BLE_API() = default;

// ws_events holds a plain function pointer, so route through a file-scope
// forwarder rather than giving it a hard dependency on this module.
static BLESerialService *g_event_service = nullptr;
static void bleEventSink(const String &frame) {
    if (g_event_service) g_event_service->notifyEvent(frame);
}

class BLEAPICallback : public NimBLEServerCallbacks {
    BLE_API *api;

    void onConnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo) override {
        pServer->updateConnParams(connInfo.getConnHandle(), 6, 24, 0, 400); // Improve latency
    };

    void onMTUChange(uint16_t MTU, NimBLEConnInfo &connInfo) override { api->update_mtu(MTU); };

    // A peer that vanishes — app reloaded, out of range, radio off — never
    // writes 0 to the CCCD, so onSubscribe is not called and the service still
    // believes somebody is listening. Every event then notifies a dead
    // characteristic: bleNotifyRetry burns 8 retries per chunk and
    // notifyChunkedTo logs a failure, for every frame, forever.
    void onDisconnect(NimBLEServer *pServer, NimBLEConnInfo &connInfo, int reason) override {
        api->on_disconnect();
    };

public:
    explicit BLEAPICallback(BLE_API *api) : api(api) {}
};

void BLE_API::setup() {
    NimBLEDevice::init("Bruce");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // 9 dBm, tweak if you want

    pServer = NimBLEDevice::createServer();
    pServer->advertiseOnDisconnect(true);
    pServer->setCallbacks(new BLEAPICallback(this));

    battery_service.setup(pServer);
    serial_service.setup(pServer);
    serialDevice = &serial_service;

    BLEAdvertising *pAdvertising = pServer->getAdvertising();
    pAdvertising->enableScanResponse(false); // Save some battery
    pAdvertising->setName("Bruc");           // Bruce is too long for adv packet len
    pAdvertising->start();

    g_event_service = &serial_service;
    registerEventSink(&bleEventSink);
}

void BLE_API::on_disconnect() { serial_service.setEventSubscribed(false); }

void BLE_API::update_mtu(uint16_t mtu) {
    battery_service.setMTU(mtu);
    serial_service.setMTU(mtu);
}

void BLE_API::end() {
    // Order matters: the sink must stop firing before the service it forwards
    // to is torn down, or a log raised mid-teardown notifies a dead
    // characteristic.
    registerEventSink(nullptr);
    g_event_service = nullptr;

    battery_service.end();
    serial_service.end();
    BLEDevice::deinit();
    serialDevice = &USBserial;
}
#endif
