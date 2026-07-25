#if !defined(LITE_VERSION)
#include "ws_events.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

static AsyncWebSocket ws("/ws");
static uint32_t wsEventId = 0;
static String deviceState = "idle";

void beginWsServer(AsyncWebServer *server) {
    ws.onEvent([](AsyncWebSocket *srv, AsyncWebSocketClient *cli, AwsEventType type,
                 void *arg, uint8_t *data, size_t len) {
        if (type == WS_EVT_CONNECT) {
            cli->text(String("{\"id\":") + wsEventId + ",\"type\":\"state\",\"device_state\":\"" + deviceState + "\"}");
        }
        // ponytail: client subscribe ignored, app re-fetches /systeminfo for resume
    });
    server->addHandler(&ws);
}

void pushWsEvent(const String &type, const String &jsonPayload) {
    if (ws.count() == 0) return;
    String frame = "{\"id\":" + String(++wsEventId) + ",\"type\":\"" + type + "\"" + jsonPayload + "}";
    ws.textAll(frame);
}

void pushWsLog(const String &line, const char *level) {
    String escaped = line;
    escaped.replace("\\", "\\\\");
    escaped.replace("\"", "\\\"");
    pushWsEvent("log", ",\"line\":\"" + escaped + "\",\"level\":\"" + level + "\"");
}

void setDeviceState(const String &state) {
    deviceState = state;
    pushWsEvent("state", ",\"device_state\":\"" + state + "\"");
}

String getDeviceState() { return deviceState; }
#endif
