#ifndef __CRASH_REPORT_H__
#define __CRASH_REPORT_H__

#include <stdint.h>
#include <stdio.h>
#include <string>

// Deliberately free of Arduino, FreeRTOS and ESP-IDF so it builds under
// [env:native]. esp_core_dump_summary_t cannot come in here; crash_commands.cpp
// copies the fields it needs into the view below.

struct CrashSummaryView {
    const char *taskName; // may be null or empty
    uint32_t excPc;
    const uint32_t *bt; // may be null, in which case depth is treated as 0
    uint32_t depth;     // read from the dump, so clamped rather than trusted
    uint32_t btCapacity;
    bool corrupted;
    const char *elfSha256; // NUL-terminated ASCII, may be null or empty
};

// Values of esp_reset_reason_t, spelled as ints so this stays IDF-free.
static inline const char *resetReasonName(int reason) {
    switch (reason) {
        case 1: return "poweron";
        case 2: return "external";
        case 3: return "sw_restart";
        case 4: return "panic";
        case 5: return "int_wdt";
        case 6: return "task_wdt";
        case 7: return "other_wdt";
        case 8: return "deepsleep";
        case 9: return "brownout";
        case 10: return "sdio";
        default: return "unknown";
    }
}

// One field per token and at most two lines, because this is read over BLE where a
// reply can truncate (ISSUE-16) and a half-received line must still be useful.
static inline std::string formatCrashSummary(const CrashSummaryView &v) {
    const char *task = (v.taskName && v.taskName[0]) ? v.taskName : "?";
    const char *elf = (v.elfSha256 && v.elfSha256[0]) ? v.elfSha256 : "?";

    uint32_t depth = v.depth;
    if (depth > v.btCapacity) depth = v.btCapacity;
    if (v.bt == nullptr) depth = 0;

    char buf[64];
    std::string out = "crash: task=";
    out += task;
    snprintf(buf, sizeof(buf), " pc=0x%08x", (unsigned)v.excPc);
    out += buf;
    out += " elf=";
    out += elf;
    snprintf(buf, sizeof(buf), " depth=%u corrupted=", (unsigned)depth);
    out += buf;
    out += v.corrupted ? "yes" : "no";

    if (depth > 0) {
        out += "\r\ncrash: bt=";
        for (uint32_t i = 0; i < depth; i++) {
            if (i) out += ' ';
            snprintf(buf, sizeof(buf), "0x%08x", (unsigned)v.bt[i]);
            out += buf;
        }
    }
    return out;
}

#endif
