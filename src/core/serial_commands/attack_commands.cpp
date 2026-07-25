#if !defined(LITE_VERSION)
#include "attack_commands.h"
#include "core/settings.h"
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
        // Pause BLE API GATT server to free DMA memory for the spam attack.
        // Without this, radioHasMemForBle() sees low DMA (BLE API holding ~15KB)
        // and tears down WiFi — killing /ws telemetry + app connectivity.
        bool wasBleApiOn = bleApiIsEnabled();
        if (wasBleApiOn) {
            enableBLEAPI();  // toggles OFF
            Serial.println("[BLE_SPAM] Paused BLE API for spam attack");
        }

        setDeviceState("ble_spam");
        FastPairExploitEngine fpEngine;
        fpEngine.spamFastPairPopups(fpType, count);
        setDeviceState("idle");

        // Resume BLE API GATT server after the attack
        if (wasBleApiOn) {
            enableBLEAPI();  // toggles ON
            Serial.println("[BLE_SPAM] Resumed BLE API after spam attack");
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
