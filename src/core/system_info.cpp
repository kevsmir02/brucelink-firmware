#include "system_info.h"
#include "core/wifi/webInterface.h" // humanReadableSize
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD.h>
#include <WiFi.h>
#include <globals.h>

String buildSystemInfoJson() {
    JsonDocument doc;
    doc["BRUCE_VERSION"] = BRUCE_VERSION;
    JsonObject sd = doc["SD"].to<JsonObject>();
    sd["free"] = humanReadableSize(SD.totalBytes() - SD.usedBytes());
    sd["used"] = humanReadableSize(SD.usedBytes());
    sd["total"] = humanReadableSize(SD.totalBytes());
    JsonObject lfs = doc["LittleFS"].to<JsonObject>();
    lfs["free"] = humanReadableSize(LittleFS.totalBytes() - LittleFS.usedBytes());
    lfs["used"] = humanReadableSize(LittleFS.usedBytes());
    lfs["total"] = humanReadableSize(LittleFS.totalBytes());

    JsonObject caps = doc["capabilities"].to<JsonObject>();
#if defined(USB_as_HID) && USB_as_HID
    caps["usb_as_hid"] = true;
#else
    caps["usb_as_hid"] = false;
#endif
#if defined(HAS_SCREEN)
    caps["has_screen"] = true;
#else
    caps["has_screen"] = false;
#endif
#if defined(LITE_VERSION)
    caps["lite_version"] = true;
#else
    caps["lite_version"] = false;
#endif
#if defined(USE_CC1101_VIA_SPI)
    caps["has_cc1101"] = true;
#else
    caps["has_cc1101"] = false;
#endif
#if defined(USE_NRF24_VIA_SPI)
    caps["has_nrf24"] = true;
#else
    caps["has_nrf24"] = false;
#endif
    caps["has_pn532"] = false;
#if defined(GPS_SERIAL_TX)
    caps["has_gps"] = true;
#else
    caps["has_gps"] = false;
#endif
#if defined(IR_TX_PINS)
    caps["has_ir"] = true;
#else
    caps["has_ir"] = false;
#endif
    caps["has_fm"] = false;
    caps["has_eth"] = false;
#if defined(BUZZ_PIN)
    caps["has_buzz"] = true;
#else
    caps["has_buzz"] = false;
#endif
#if defined(HAS_RGB_LED)
    caps["has_rgb_led"] = true;
#else
    caps["has_rgb_led"] = false;
#endif
#if defined(MIC_INMP441)
    caps["has_mic"] = true;
#else
    caps["has_mic"] = false;
#endif

    doc["battery_pct"] = getBattery();
    doc["charging"] = isCharging();
    doc["wifi_mode"] = (int)WiFi.getMode();
    doc["ip"] = WiFi.localIP().toString();
    doc["free_heap"] = (int)ESP.getFreeHeap();
    doc["psram"] = psramFound();

    String body;
    serializeJson(doc, body);
    return body;
}
