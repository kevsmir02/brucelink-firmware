#if !defined(LITE_VERSION)
#include "attack_commands.h"
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

    FastPairPopupType fpType;
    bool useFastPair = true;
    if (typeStr == "fastpair_regular")      fpType = FP_POPUP_REGULAR;
    else if (typeStr == "fastpair_fun")     fpType = FP_POPUP_FUN;
    else if (typeStr == "fastpair_prank")   fpType = FP_POPUP_PRANK;
    else if (typeStr == "fastpair_custom")  fpType = FP_POPUP_CUSTOM;
    else { useFastPair = false; }

    if (useFastPair) {
        // spamFastPairPopups() calls BLEStateManager::initBLE, whose radioHasMemForBle()
        // guard tears down the WiFi AP to free the ~15KB contiguous internal DMA the BT
        // controller needs. On this board PSRAM can't back BT-controller DMA, so the AP
        // and BLE cannot hold their DMA buffers simultaneously — the teardown is a
        // crash-prevention guard, NOT a bug (see radio_mem.h). Bypassing it to keep the
        // AP up would half-init esp_bt_controller_init and crash the device — that path
        // was tried and reverted (commit e2631370). Instead we time-share the radio:
        // let the guard drop the AP for the short spam, then bring the AP back up so the
        // companion app / Web UI reconnects. By the time spamFastPairPopups() returns,
        // its AutoCleanup has deinit'd BLE, so the DMA is free and the AP restart is safe.
        wifi_mode_t wifiModeBefore = WiFi.getMode();
        bool apWasUp = (wifiModeBefore == WIFI_MODE_AP || wifiModeBefore == WIFI_MODE_APSTA);

        setDeviceState("ble_spam");
        FastPairExploitEngine fpEngine;
        fpEngine.spamFastPairPopups(fpType, count);
        setDeviceState("idle");

        // Auto-recover the AP if the guard tore it down for the spam.
        if (apWasUp) {
            wifi_mode_t wifiModeAfter = WiFi.getMode();
            bool apStillUp = (wifiModeAfter == WIFI_MODE_AP || wifiModeAfter == WIFI_MODE_APSTA);
            if (!apStillUp) {
                Serial.println("[BLE_SPAM] Restoring WiFi AP after spam");
                WiFi.mode(WIFI_AP);
                _setupAP();
            }
        }
        return true;
    }
    if (typeStr == "menu") {
        spamMenu();
        return true;
    }
    serialDevice->println("usage: blespam <fastpair_regular|fastpair_fun|fastpair_prank|fastpair_custom|menu> <count>");
    return false;
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
