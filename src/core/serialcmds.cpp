#include "serialcmds.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "utils.h"
#include <globals.h>
#if !defined(LITE_VERSION)
#include "core/wifi/ws_events.h"
#endif

QueueHandle_t cmdQueue = nullptr;
QueueHandle_t rspQueue = nullptr;
TaskHandle_t serialcmdsTaskHandle;

struct CmdPacket {
    char text[512]; // command size
};
bool parseSerialCommand(const String &command, bool waitForResponse) {
    if (!cmdQueue || !rspQueue) {
        Serial.println("Command or response queue not initialized");
        return false;
    }
    CmdPacket packet;
    memset(&packet, 0, sizeof(packet));
    strncpy(packet.text, command.c_str(), sizeof(packet.text) - 1);

    // Enqueue the command packet for processing
    if (xQueueSend(cmdQueue, &packet, 0) != pdTRUE) {
        Serial.println("Failed to send command to queue");
        return false;
    }
    if (!waitForResponse) { return true; }
    // Wait for the response
    bool result;
    if (xQueueReceive(rspQueue, &result, pdMS_TO_TICKS(20))) { return result; }
    Serial.println("Failed to receive command response");
    return false;
}

void handleSerialCommands(SerialCli &serialCli) {
    CmdPacket packet;
    if (cmdQueue && rspQueue) {
        if (xQueueReceive(cmdQueue, &packet, 0) == pdTRUE) {
#if !defined(LITE_VERSION)
            pushWsLog(String("COMMAND: ") + packet.text, "info");
#endif
            bool result = serialCli.parse(String(packet.text));
            xQueueSend(rspQueue, &result, 0);
            Serial.println("COMMAND: " + String(packet.text));
            Serial.printf("[CLI] Result: %s\n", result ? "TRUE" : "FALSE");
#if !defined(LITE_VERSION)
            pushWsLog(String("[CLI] Result: ") + (result ? "TRUE" : "FALSE"), "info");
#endif
        }
    }
    // hasLine(), not available(): on BLE a command can arrive split across
    // several characteristic writes, and acting on a partial one would parse a
    // fragment as a whole command.
    if (!serialDevice->hasLine('\n')) return;

    String cmd_str = serialDevice->readStringUntil('\n');
    Serial.println("COMMAND: " + cmd_str);
#if !defined(LITE_VERSION)
    pushWsLog(String("COMMAND: ") + cmd_str, "info");
#endif
    bool result = serialCli.parse(cmd_str);
#if !defined(LITE_VERSION)
    pushWsLog(String("[CLI] Result: ") + (result ? "TRUE" : "FALSE"), "info");
#endif
    serialDevice->print("# "); // prompt

    // forced menu redrawn if the command is not "nav" or "option"
    // it allows navigation commands to be executed without returning to the menu, while other commands will
    // return to the menu after execution.
    String cmd_trimmed = cmd_str;
    cmd_trimmed.trim();
    if (!cmd_trimmed.startsWith("nav") && !cmd_trimmed.startsWith("option")) { backToMenu(); }
}

void _serialCmdsTaskLoop(void *pvParameters) {
    Serial.begin(115200);
    while (1) {
        handleSerialCommands(serialCli);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void startSerialCommandsHandlerTask(bool initQueues) {
    if (initQueues) {
        cmdQueue = xQueueCreate(2, sizeof(CmdPacket));
        rspQueue = xQueueCreate(2, sizeof(bool));
    }

    xTaskCreatePinnedToCore(
        _serialCmdsTaskLoop,         // Function to implement the task
        "serialcmds",                // Name of the task (any string)
        SERIAL_CMDS_TASK_STACK_SIZE, // Stack size in bytes
        NULL, // This is a pointer to the parameter that will be passed to the new task. We are not using it
              // here and therefore it is set to NULL.
        2,    // Priority of the task
        &serialcmdsTaskHandle, // Task handle (optional, can be NULL).
#if SOC_CPU_CORES_NUM > 1
        1 // Core where the task should run. By default, all your Arduino code runs on Core 1 and the Wi-Fi
          // and RF functions
#else
        0 // Core where the task should run. ESP32-C5 has only one core
#endif
    ); // (these are usually hidden from the Arduino environment) use the Core 0.
    if (!serialcmdsTaskHandle) { Serial.println("Failed to create Serial Commands Handler task"); }
}
