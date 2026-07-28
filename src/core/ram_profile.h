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
#define RAM_LOG_SAMPLER(intervalMs) ramProfileStartSampler(intervalMs)
#define RAM_LOGF(...) ramProfileLogf(__VA_ARGS__)

#else

#define RAM_LOG(stage) ((void)0)
#define RAM_LOG_SAMPLER(intervalMs) ((void)0)
#define RAM_LOGF(...) ((void)0)

#endif // ENABLE_RAM_LOGGING

#endif // __RAM_PROFILE_H__
