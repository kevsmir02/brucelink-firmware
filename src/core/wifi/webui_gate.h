#ifndef __WEBUI_GATE_H__
#define __WEBUI_GATE_H__

#include <stdint.h>
#include <stdio.h>
#include <string>

// Deliberately free of Arduino, FreeRTOS and ESP-IDF so it builds under
// [env:native]. lwip/tcpbase.h and esp_heap_caps.h cannot come in here; the
// caller reads the socket state and the heap and copies the figures into the
// view below. Same arrangement as core/crash_report.h.

enum class WebUiStartResult : uint8_t {
    Started = 0,
    // Gate A or gate B. wifiConnectMenu() returns a bare bool, so distinguishing
    // "refused for memory" from "softAP failed" here would be a guess; each gate
    // logs its own reason instead.
    WifiBringUpFailed,
    RefusedLowDmaPreAlloc,
    FailedNotListening,
};

struct WebUiStartReport {
    WebUiStartResult result;
    uint32_t dmaBlock; // largest contiguous DMA-capable block at the deciding moment
    uint32_t required; // carried so a truncated line is still self-describing
    uint8_t tcpState;  // AsyncWebServer::state() as read; 0 where not applicable
    bool apMode;
};

// lwIP tcp_state::LISTEN. A literal rather than an include, to keep this header
// buildable off-device; the value is fixed by the on-wire TCP state machine.
static constexpr uint8_t WEBUI_TCP_LISTEN = 1;

static inline bool webUiListening(uint8_t tcpState) { return tcpState == WEBUI_TCP_LISTEN; }

static inline bool webUiDmaSufficient(uint32_t block, uint32_t required) { return block >= required; }

static inline const char *webUiResultSlug(WebUiStartResult r) {
    switch (r) {
        case WebUiStartResult::Started: return "started";
        case WebUiStartResult::WifiBringUpFailed: return "wifi_bringup_failed";
        case WebUiStartResult::RefusedLowDmaPreAlloc: return "low_dma_pre_alloc";
        case WebUiStartResult::FailedNotListening: return "not_listening";
    }
    return "unknown";
}

// One line, one field per token, because this is read over BLE where a reply can
// truncate (ISSUE-16) and a half-received line must still be useful.
static inline std::string formatWebUiStartReport(const WebUiStartReport &r) {
    char buf[96];
    std::string out = "webui: ";
    out += webUiResultSlug(r.result);
    out += r.apMode ? " mode=ap" : " mode=sta";
    snprintf(
        buf,
        sizeof(buf),
        " dma=%u required=%u tcp_state=%u",
        (unsigned)r.dmaBlock,
        (unsigned)r.required,
        (unsigned)r.tcpState
    );
    out += buf;
    return out;
}

#endif
