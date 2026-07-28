#include "ram_profile.h"

#if defined(ENABLE_RAM_LOGGING)

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdarg.h>

// On boards built with ARDUINO_USB_CDC_ON_BOOT (smoochiee-board among them)
// `Serial` is the native USB-CDC port, NOT the UART bridge. A laptop attached
// to the UART bridge therefore sees the ESP_LOG stream — which goes to UART0 —
// but never a single Serial.printf, so every RAM_LOG below was landing on a
// port that was not plugged in. That is why boot heap figures have been
// invisible on the bench.
//
// Mirror to UART0 as well. TX only: UART0's RX pin is GPIO44, which is
// BUZZ_PIN on this board and already has LEDC attached to it by
// startup_sound(). Passing rxPin = -1 leaves that pin alone.
#if defined(ARDUINO_USB_CDC_ON_BOOT)
#define RAM_LOG_UART0_TX_PIN 43

static Print *ramProfileMirror() {
    static bool started = false;
    if (!started) {
        Serial0.begin(115200, SERIAL_8N1, -1, RAM_LOG_UART0_TX_PIN);
        started = true;
    }
    return &Serial0;
}
#else
static Print *ramProfileMirror() { return nullptr; }
#endif

void ramProfileLog(const char *stage) {
    // Default (8-bit capable) heap as reported by the Arduino layer.
    uint32_t freeHeap = ESP.getFreeHeap();
    uint32_t minFreeEver = ESP.getMinFreeHeap();

    // INTERNAL DRAM is the metric that matters for Wi-Fi/BLE init on boards
    // without PSRAM: free total and, crucially, the LARGEST CONTIGUOUS block.
    uint32_t internalFree = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    uint32_t internalLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);

    // DMA-capable internal block (some Wi-Fi/BLE buffers require this).
    uint32_t dmaLargest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);

    uint32_t psramFree = ESP.getFreePsram();

    char line[256];
    snprintf(
        line,
        sizeof(line),
        "[RAMLOG] t=%6lums stage=%-20s | heap free=%7u minEver=%7u | internal free=%7u "
        "largest=%7u dma=%7u | psram found=%d free=%8u",
        millis(),
        stage,
        freeHeap,
        minFreeEver,
        internalFree,
        internalLargest,
        dmaLargest,
        (int)psramFound(),
        psramFree
    );

    Serial.println(line);
    Serial.flush();

    if (Print *mirror = ramProfileMirror()) {
        mirror->println(line);
        mirror->flush();
    }
}

// Arbitrary diagnostic line on the same dual-port path as RAM_LOG. Exists
// because plain Serial.printf is invisible on USB-CDC boards (see above), so any
// bring-up diagnostic written the obvious way cannot be read on the bench.
void ramProfileLogf(const char *fmt, ...) {
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    Serial.println(line);
    Serial.flush();

    if (Print *mirror = ramProfileMirror()) {
        mirror->println(line);
        mirror->flush();
    }
}

// Sampling from its own task rather than from loop() is deliberate. Bruce's apps
// each run their own blocking loop — evilportal, karma, a BLE attack — so a
// sampler driven from loop() goes silent for exactly the windows we most need
// measured. A task keeps emitting throughout, which is what makes a steady-state
// footprint distinguishable from a genuine leak.
static void ramProfileSamplerTask(void *arg) {
    const uint32_t intervalMs = (uint32_t)(uintptr_t)arg;
    for (;;) {
        ramProfileLog("sample");
        vTaskDelay(pdMS_TO_TICKS(intervalMs));
    }
}

void ramProfileStartSampler(uint32_t intervalMs) {
    static TaskHandle_t handle = nullptr;
    if (handle) return;
    // 4 KB: snprintf into a 256-byte frame plus the Serial write path. The
    // battery task needed enlarging for the same reason (dd2ef38d), so this is
    // sized with margin rather than trimmed to the observed minimum.
    xTaskCreate(ramProfileSamplerTask, "ramprof", 4096, (void *)(uintptr_t)intervalMs, 1, &handle);
}

#endif // ENABLE_RAM_LOGGING
