#ifndef __PORTAL_CAP_H__
#define __PORTAL_CAP_H__

#include <stdint.h>

// Deliberately free of Arduino and FreeRTOS so it builds under [env:native],
// where almost nothing else in this feature can be tested.

// maxMs == 0 means no cap.
static inline bool portalCapExpired(uint32_t startedMs, uint32_t nowMs, uint32_t maxMs) {
    if (maxMs == 0) return false;
    // Unsigned wraparound makes this correct across the ~49.7 day millis() rollover.
    return (uint32_t)(nowMs - startedMs) >= maxMs;
}

// Returns 0 when uncapped; callers must check maxMs to tell that from "expired".
static inline uint32_t portalCapRemainingMs(uint32_t startedMs, uint32_t nowMs, uint32_t maxMs) {
    if (maxMs == 0) return 0;
    uint32_t elapsed = (uint32_t)(nowMs - startedMs);
    return elapsed >= maxMs ? 0 : maxMs - elapsed;
}

#endif
