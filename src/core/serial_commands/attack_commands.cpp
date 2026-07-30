#if !defined(LITE_VERSION)
#include "attack_commands.h"
#include "core/ram_profile.h"
#include "core/settings.h"
#include "core/wifi/wifi_common.h"
#include "core/wifi/ws_events.h"
#include "modules/wifi/evil_portal.h"
#include "modules/wifi/evil_portal_bg.h"
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

    // Flags are handled before the start path so -off and -status can never
    // bring a portal up as a side effect.
    if (cmd.getArgument("off").isSet()) { return evilPortalBgStop(); }
    if (cmd.getArgument("status").isSet()) {
        serialDevice->println(evilPortalBgStatus());
        return true;
    }

    String ssid = cmd.getArgument("ssid").getValue();
    String chStr = cmd.getArgument("channel").getValue();
    String templateFile = cmd.getArgument("template").getValue();
    ssid.trim();
    chStr.trim();
    templateFile.trim();
    if (ssid.isEmpty()) ssid = "Free Wifi";
    uint8_t channel = (uint8_t)chStr.toInt();
    if (channel < 1 || channel > 13) channel = 6;

    if (cmd.getArgument("bg").isSet()) {
        String durStr = cmd.getArgument("duration").getValue();
        durStr.trim();
        // Only an explicit run of digits may reach the cap, because 0 means
        // "unlimited" and toInt() also answers 0 for "-5" and for "abc". Mapping a
        // malformed duration onto the uncapped case would disarm the one recovery
        // path that survives `ble api off`.
        bool numeric = !durStr.isEmpty();
        for (unsigned i = 0; i < durStr.length(); i++) {
            if (!isdigit((unsigned char)durStr[i])) numeric = false;
        }
        if (!numeric) {
            serialDevice->println(
                "ERROR: -duration must be a whole number of seconds (0 = unlimited), got '" +
                durStr + "'"
            );
            return false;
        }
        return evilPortalBgStart(ssid, channel, templateFile, (uint32_t)durStr.toInt());
    }

    // The blocking portal holds the serial task, which is what pumps the background
    // one — so starting it here would freeze the background portal's duration cap
    // and then tear down its AP through the shared DNS server and wifiDisconnect().
    if (evilPortalBgRunning()) {
        serialDevice->println(
            "ERROR: a background portal is already running. Stop it with "
            "'evilportal -off' before starting the blocking portal."
        );
        return false;
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

// Every entry point below is `void` upstream, so there is genuinely no return value
// to propagate — the old bare `return true` was the only thing available, not
// laziness. What *is* knowable is that the callback ran to completion, how long it
// took, and what state the radio and heap were left in. Emitting that as a frame
// gives the app the outcome telemetry ISSUE-7 asks for, and lets it recognise the
// failure shape that started this: `reverseshell` reported success 30 ms after its
// AP creation had failed outright, and an instant return is exactly what that looks
// like from outside.
//
// `outcome` is deliberately "completed", never "success". These verbs open an
// interactive menu; finishing one says the operator left it, not that an attack
// worked. Anything stronger would repeat the lie in a new field.
static void pushAttackResult(const char *verb, const char *outcome, uint32_t elapsedMs) {
    pushWsEvent(
        "attack_result",
        String(",\"verb\":\"") + verb + "\",\"outcome\":\"" + outcome +
            "\",\"elapsed_ms\":" + String(elapsedMs) + ",\"wifi_mode\":" +
            String((int)WiFi.getMode()) + ",\"free_heap\":" + String(ESP.getFreeHeap())
    );
}

static uint32_t runInteractiveAttack(const char *verb, void (*entry)()) {
    const uint32_t t0 = millis();
    setDeviceState(verb);
    entry();
    setDeviceState("idle");
    pushAttackResult(verb, "completed", millis() - t0);
    return true;
}

uint32_t karmaCmdCallback(cmd *c) { return runInteractiveAttack("karma", karma_setup); }

uint32_t deauthCmdCallback(cmd *c) {
    Command cmd(c);
    String target = cmd.getArgument("target").getValue();
    target.trim();
    // The argument was accepted and then dropped on the floor (ISSUE-5): the verb
    // calls wifi_atk_menu(), which takes no target and makes the operator pick one on
    // the device. Refuse it rather than letting a caller believe it aimed the attack.
    if (!target.isEmpty()) {
        serialDevice->println(
            "ERROR: 'deauth <target>' is not supported — wifi_atk_menu() selects the "
            "target on the device. Run 'deauth' with no argument."
        );
        pushAttackResult("deauth", "rejected_unsupported_target", 0);
        return false;
    }
    return runInteractiveAttack("deauth", wifi_atk_menu);
}

uint32_t blesnifferCmdCallback(cmd *c) { return runInteractiveAttack("blesniffer", BleSuiteMenu); }

uint32_t apInfoCmdCallback(cmd *c) { return runInteractiveAttack("ap_info", displayAPInfo); }

uint32_t reverseshellCmdCallback(cmd *c) {
    // The one verb here with a knowable outcome, now that ReverseShell() reports
    // whether its AP came up.
    const uint32_t t0 = millis();
    setDeviceState("reverseshell");
    const bool ok = ReverseShell();
    setDeviceState("idle");
    if (!ok) {
        serialDevice->println("ERROR: reverseshell could not start its AP");
        pushAttackResult("reverseshell", "ap_failed", millis() - t0);
        return false;
    }
    pushAttackResult("reverseshell", "completed", millis() - t0);
    return true;
}

uint32_t pwngridCmdCallback(cmd *c) { return runInteractiveAttack("pwngrid", brucegotchi_start); }

void createAttackCommands(SimpleCLI *cli) {
    Command ble = cli->addCompositeCmd("ble");
    Command bleApi = ble.addCommand("api", bleApiCmdCallback);
    bleApi.addPosArg("state", "on");

    Command evilportal = cli->addCommand("evilportal", evilportalCmdCallback);
    evilportal.addPosArg("ssid", "Free Wifi");
    evilportal.addPosArg("channel", "6");
    evilportal.addPosArg("template", "");
    evilportal.addFlagArg("bg");
    evilportal.addFlagArg("off");
    evilportal.addFlagArg("status");
    // 0 disables the cap. The default is deliberately finite: with ble api off
    // nothing can reach the device to stop a portal, so the clock is the only
    // recovery path.
    evilportal.addArg("duration", "600");

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
