#if !defined(LITE_VERSION)
#include "attack_commands.h"
#include "core/ram_profile.h"
#include "core/settings.h"
#include "core/wifi/wifi_common.h"
#include "core/wifi/ws_events.h"
#include "modules/wifi/evil_portal.h"
#include "modules/ble/BLE_Suite.h"
#include "modules/ble/ble_spam.h"
#include "modules/wifi/karma_attack.h"
#include "modules/wifi/wifi_atks.h"
#include "modules/wifi/ap_info.h"
#include "modules/reverseShell/reverseShell.h"
#include "modules/pwnagotchi/pwnagotchi.h"
#include <SimpleCLI.h>
#include <globals.h>

uint32_t bleApiCmdCallback(cmd *c) {
    Command cmd(c);
    String state = cmd.getArgument("state").getValue();
    state.trim();
    if (state == "on") {
        if (!bleApiIsEnabled()) enableBLEAPI();
        serialDevice->println(String("BLE API ") + (bleApiIsEnabled() ? "ON" : "OFF"));
        return bleApiIsEnabled();
    } else if (state == "off") {
        if (bleApiIsEnabled()) enableBLEAPI();
        serialDevice->println(String("BLE API ") + (bleApiIsEnabled() ? "ON" : "OFF"));
        return !bleApiIsEnabled();
    }
    serialDevice->println("usage: ble api on|off");
    return false;
}

uint32_t evilportalCmdCallback(cmd *c) {
    Command cmd(c);
    String ssid = cmd.getArgument("ssid").getValue();
    String chStr = cmd.getArgument("channel").getValue();
    String templateFile = cmd.getArgument("template").getValue();
    ssid.trim();
    chStr.trim();
    templateFile.trim();
    if (ssid.isEmpty()) ssid = "Free Wifi";
    uint8_t channel = (uint8_t)chStr.toInt();
    if (channel < 1 || channel > 13) channel = 6;
    // Default gateway to 192.168.4.1 for phone captive-portal compatibility
    // (172.0.0.1 breaks Android/iOS auto-detection — phones expect 192.168.4.1)
    if (bruceConfig.evilPortalGatewayIp.isEmpty()) {
        bruceConfig.evilPortalGatewayIp = "192.168.4.1";
    }
    setDeviceState("portal");
    EvilPortal(ssid, channel, false, false, true, false, templateFile);
    setDeviceState("idle");
    return true;
}

uint32_t blespamCmdCallback(cmd *c) {
    Command cmd(c);
    String typeStr = cmd.getArgument("type").getValue();
    String countStr = cmd.getArgument("count").getValue();
    typeStr.trim();
    countStr.trim();
    int count = countStr.toInt();
    if (count < 1) count = 10;

    // The interactive on-device menu drives its own radio lifecycle — no time-share.
    if (typeStr == "menu") {
        spamMenu();
        return true;
    }

    // Route the type to an engine: fastpair_* -> the FastPair popup engine;
    // apple/android/ibeacon/samsung/windows/random -> the ble_spam engine.
    FastPairPopupType fpType;
    bool useFastPair = true;
    if (typeStr == "fastpair_regular")      fpType = FP_POPUP_REGULAR;
    else if (typeStr == "fastpair_fun")     fpType = FP_POPUP_FUN;
    else if (typeStr == "fastpair_prank")   fpType = FP_POPUP_PRANK;
    else if (typeStr == "fastpair_custom")  fpType = FP_POPUP_CUSTOM;
    else { useFastPair = false; }

    // Validate the verb BEFORE tearing the AP down, so an unknown type doesn't
    // flap the WiFi AP for nothing.
    if (!useFastPair && !bleSpamIsKnownAttackName(typeStr)) {
        serialDevice->println(
            "usage: blespam <fastpair_regular|fastpair_fun|fastpair_prank|fastpair_custom|"
            "apple|android|ibeacon|samsung|windows|random|menu> <count>"
        );
        return false;
    }

    // Both engines monopolize the radio. On no-PSRAM boards the BT controller and the
    // WiFi AP can't hold their DMA buffers at once, so bringing BLE up tears the AP
    // down (radioHasMemForBle, see radio_mem.h) — a crash-prevention guard, NOT a bug.
    // Bypassing it half-inits esp_bt_controller_init and crashes the device; that path
    // was tried and reverted (commit e2631370). Instead we time-share the radio: let the
    // guard drop the AP for the short spam, then bring it back up so the companion app /
    // Web UI reconnects. By the time the spam returns, BLE is deinit'd and the DMA is
    // free, so the AP restart is safe.
    wifi_mode_t wifiModeBefore = WiFi.getMode();
    bool apWasUp = (wifiModeBefore == WIFI_MODE_AP || wifiModeBefore == WIFI_MODE_APSTA);

    // Warn the companion app over the link it is about to lose, THEN drop the GATT
    // server before either spam engine touches NimBLE. Once the API is down the
    // memory guard has enough DMA to leave the AP alone, so control swaps to WiFi
    // for the duration rather than disappearing entirely.
    bool bleApiWasUp = bleApiIsEnabled();
    if (bleApiWasUp) {
        serialDevice->println("blespam: BLE control link suspended — reconnect over WiFi");
        serialDevice->endOfResponse();
        bleApiSuspend();
    }

    setDeviceState("ble_spam");
    RAM_LOG("swap attack-pre");
    if (useFastPair) {
        FastPairExploitEngine fpEngine;
        fpEngine.spamFastPairPopups(fpType, count);
    } else {
        bleSpamRunAttackByName(typeStr, count);
    }
    RAM_LOG("swap attack-post");
    setDeviceState("idle");

    // Auto-recover the AP if the guard tore it down for the spam. With the API
    // suspended first this should now be a no-op — the guard passes without
    // touching WiFi — so a "Restoring WiFi AP" line here means the swap did not
    // buy enough contiguous DMA and is worth investigating rather than ignoring.
    if (apWasUp) {
        wifi_mode_t wifiModeAfter = WiFi.getMode();
        bool apStillUp = (wifiModeAfter == WIFI_MODE_AP || wifiModeAfter == WIFI_MODE_APSTA);
        RAM_LOG(apStillUp ? "swap ap-survived" : "swap ap-lost");
        if (!apStillUp) {
            Serial.println("[BLE_SPAM] Restoring WiFi AP after spam");
            WiFi.mode(WIFI_AP);
            _setupAP();
            RAM_LOG("swap ap-restored");
        }
    }

    // Rebuild the control link so the app can swap back to BLE and replay from
    // lastEventId.
    if (bleApiWasUp) bleApiResume();
    return true;
}

uint32_t karmaCmdCallback(cmd *c) {
    karma_setup();
    return true;
}

uint32_t deauthCmdCallback(cmd *c) {
    wifi_atk_menu();
    return true;
}

uint32_t blesnifferCmdCallback(cmd *c) {
    BleSuiteMenu();
    return true;
}

uint32_t apInfoCmdCallback(cmd *c) {
    displayAPInfo();
    return true;
}

uint32_t reverseshellCmdCallback(cmd *c) {
    ReverseShell();
    return true;
}

uint32_t pwngridCmdCallback(cmd *c) {
    brucegotchi_start();
    return true;
}

void createAttackCommands(SimpleCLI *cli) {
    Command ble = cli->addCompositeCmd("ble");
    Command bleApi = ble.addCommand("api", bleApiCmdCallback);
    bleApi.addPosArg("state", "on");

    Command evilportal = cli->addCommand("evilportal", evilportalCmdCallback);
    evilportal.addPosArg("ssid", "Free Wifi");
    evilportal.addPosArg("channel", "6");
    evilportal.addPosArg("template", "");

    Command blespam = cli->addCommand("blespam", blespamCmdCallback);
    blespam.addPosArg("type", "fastpair_regular");
    blespam.addPosArg("count", "10");

    cli->addCommand("karma", karmaCmdCallback);
    Command deauth = cli->addCommand("deauth", deauthCmdCallback);
    deauth.addPosArg("target", "");
    cli->addCommand("blesniffer", blesnifferCmdCallback);
    cli->addCommand("ap_info", apInfoCmdCallback);
    cli->addCommand("reverseshell", reverseshellCmdCallback);
    cli->addCommand("pwngrid", pwngridCmdCallback);
}
#endif
