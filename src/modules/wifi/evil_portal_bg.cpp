#if !defined(LITE_VERSION)
#include "evil_portal_bg.h"
#include "core/radio_mem.h"
#include "core/wifi/ws_events.h"
#include "modules/wifi/evil_portal.h"
#include "modules/wifi/portal_cap.h"
#include <globals.h>
#include <new>

static EvilPortal *bgPortal = nullptr;
static uint32_t bgStartedMs = 0;
static uint32_t bgMaxMs = 0;
static int bgLastCredCount = 0;
static String bgSsid = "";
static uint8_t bgChannel = 0;

bool evilPortalBgRunning() { return bgPortal != nullptr; }

// Whether an armed BLE stack and the portal AP fit together is the open question
// this feature cannot answer by reasoning, so every reply carries the numbers.
static String heapReport() {
    return String("free_heap:") + String(ESP.getFreeHeap()) + " dma_block:" +
           String((unsigned)radioLargestDmaBlock());
}

// announceOnCli is false for the duration cap, which fires from the tick while the
// serial task may be mid-command. Writing to serialDevice there would land inside
// another reply and break its 0x04 EOT framing.
static void stopPortal(bool announceOnCli) {
    // The /ssid route renames a running portal, so bgSsid — captured at start — is not
    // the name the AP was actually broadcasting. Read it off the portal before deleting.
    String liveSsid = bgPortal->getApName();
    bgPortal->shutdown();
    delete bgPortal;
    bgPortal = nullptr;
    setDeviceState("idle");
    pushWsLog("portal stopped: " + liveSsid, "info");
    if (announceOnCli) serialDevice->println("portal '" + liveSsid + "' stopped. " + heapReport());
    bgSsid = "";
    bgChannel = 0;
    bgMaxMs = 0;
    bgLastCredCount = 0;
}

bool evilPortalBgStart(
    const String &ssid, uint8_t channel, const String &templateFile, uint32_t maxSeconds
) {
    if (bgPortal != nullptr) {
        serialDevice->println("ERROR: portal already running on '" + bgSsid + "'");
        return false;
    }

    // maxSeconds * 1000 must fit in uint32_t. The duration cap is the only recovery
    // path left once the BLE API is off, so letting an absurd value wrap into a
    // near-immediate stop would silently defeat the one safety net this feature has.
    const uint32_t kMaxCapSeconds = 0xFFFFFFFFUL / 1000UL;
    if (maxSeconds > kMaxCapSeconds) maxSeconds = kMaxCapSeconds;

    // No gateway default is applied here. EvilPortal::setup() resolves it from
    // bruceConfig.evilPortalGatewayIp and falls back to 172.0.0.1 itself, which two
    // handsets confirmed does not break captive-portal auto-detection (ISSUE-27).

    EvilPortal *portal =
        new (std::nothrow) EvilPortal(ssid, channel, false, false, true, true, templateFile);
    if (portal == nullptr) {
        serialDevice->println("ERROR: out of memory starting portal. " + heapReport());
        return false;
    }
    if (!portal->isReady()) {
        portal->shutdown();
        delete portal;
        serialDevice->println("ERROR: portal setup failed, not on air. " + heapReport());
        return false;
    }

    // isReady() alone cannot catch a failed AP here: setup()'s autoMode branch
    // (evil_portal.cpp) returns true on every path, and beginAP() swallows a failed
    // WiFi.softAP() into a Serial-only diagnostic that never reaches this board's
    // console. apOnAir() carries that discarded return value out instead.
    if (!portal->apOnAir()) {
        // beginAP() no longer starts DNS or HTTP behind a failed softAP() (ISSUE-28), so
        // there is no bound port-53 singleton to release here. It does switch radio mode
        // before it can know whether softAP() worked, and that state is real: the radio
        // is left in AP mode. shutdown() does not restore the previous mode despite
        // appearances — its WiFi.mode(_originalWifiMode) is immediately undone by
        // wifiDisconnect(), which forces WIFI_OFF (wifi_common.cpp:159). Off is still
        // the right outcome here; it is just not a restore.
        portal->shutdown();
        delete portal;
        serialDevice->println("ERROR: portal did not come up, softAP failed. " + heapReport());
        return false;
    }

    bgPortal = portal;
    bgSsid = ssid;
    bgChannel = channel;
    bgStartedMs = millis();
    bgMaxMs = maxSeconds * 1000UL;
    bgLastCredCount = 0;

    setDeviceState("portal");
    pushWsLog("portal started: " + ssid + " ch" + String(channel), "info");
    serialDevice->println(
        "portal '" + ssid + "' ch" + String(channel) + " started, cap " +
        (maxSeconds ? String(maxSeconds) + "s" : String("unlimited")) + ". " + heapReport()
    );
    return true;
}

bool evilPortalBgStop() {
    if (bgPortal == nullptr) {
        serialDevice->println("no background portal running");
        return false;
    }
    stopPortal(true);
    return true;
}

String evilPortalBgStatus() {
    if (bgPortal == nullptr) return "portal: stopped";
    uint32_t now = millis();
    // The portal object outlives its AP: restartWiFi() can lose softAP() mid-run and
    // leave _servicesUp false while bgPortal stays non-null. Keying the state off the
    // pointer alone reported a portal with no AP, serving nothing, as "running" with a
    // live cap counting down (ISSUE-33).
    bool apUp = bgPortal->apOnAir();
    bool svcUp = bgPortal->servicesUp();
    String out = String("portal: ") + ((apUp && svcUp) ? "running" : "degraded") +
                 " ap:" + (apUp ? "up" : "down") + " services:" + (svcUp ? "up" : "down") +
                 " ssid:" + bgPortal->getApName() + " ch:" + String(bgChannel) +
                 " uptime_s:" + String((now - bgStartedMs) / 1000) +
                 " creds:" + String(bgPortal->getCredentialCount());
    if (bgMaxMs == 0) out += " cap:unlimited";
    else out += " cap_remaining_s:" + String(portalCapRemainingMs(bgStartedMs, now, bgMaxMs) / 1000);
    return out + " " + heapReport();
}

void evilPortalBgTick() {
    if (bgPortal == nullptr) return;

    bgPortal->processRequests();

    int creds = bgPortal->getCredentialCount();
    if (creds > bgLastCredCount) {
        bgLastCredCount = creds;
        pushWsLog("portal captured credentials (" + String(creds) + " total)", "warn");
    }

    if (portalCapExpired(bgStartedMs, millis(), bgMaxMs)) {
        pushWsLog("portal duration cap reached: " + bgSsid, "info");
        stopPortal(false);
    }
}
#endif
