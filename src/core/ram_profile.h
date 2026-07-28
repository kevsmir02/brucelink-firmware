#ifndef __RAM_PROFILE_H__
#define __RAM_PROFILE_H__

// Usage: RAM_LOG("stage-name"); at each boot stage you want to measure.

#if defined(ENABLE_RAM_LOGGING)

#include <stdint.h>

// Logs, over Serial, the current heap/PSRAM state and elapsed time for a named
// boot stage. Focus is on INTERNAL DRAM (free + largest contiguous block),
// which is what Wi-Fi/BLE need and what gets exhausted on non-PSRAM boards.
void ramProfileLog(const char *stage);

// Starts a background task that emits a "sample" line every intervalMs, so the
// heap can be watched as a time series while a blocking app holds the main loop.
void ramProfileStartSampler(uint32_t intervalMs);

// printf-style diagnostic on the same dual-port path, for bring-up logging that
// would otherwise be invisible on USB-CDC boards.
void ramProfileLogf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

#define RAM_LOG(stage) ramProfileLog(stage)
#define RAM_LOGF(...) ramProfileLogf(__VA_ARGS__)

// The sampler is opt-in SEPARATELY from the rest of RAM logging, because it is
// the only part with a standing cost: xTaskCreate takes its 4 KB stack from
// internal DRAM and never gives it back. On this board the fully-loaded margin
// (BLE API + WiFi AP + WebUI) is roughly 2.5 KB of contiguous DMA, so a
// permanent 4 KB diagnostic task is larger than the headroom it is measuring —
// enough on its own to stop a station associating with the AP.
// Stage markers and RAM_LOGF stay always-on: they cost nothing at rest and they
// are what makes failures on this board visible at all.
// Enable with -D ENABLE_RAM_SAMPLER=1 when you specifically want a time series.
#if defined(ENABLE_RAM_SAMPLER)
#define RAM_LOG_SAMPLER(intervalMs) ramProfileStartSampler(intervalMs)
#else
#define RAM_LOG_SAMPLER(intervalMs) ((void)0)
#endif

#else

#define RAM_LOG(stage) ((void)0)
#define RAM_LOG_SAMPLER(intervalMs) ((void)0)
#define RAM_LOGF(...) ((void)0)

#endif // ENABLE_RAM_LOGGING

#endif // __RAM_PROFILE_H__
