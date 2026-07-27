#if !defined(LITE_VERSION)
#include "ws_events.h"
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>

// addHandler() TAKES OWNERSHIP: AsyncWebServer::_handlers is a
// std::list<std::unique_ptr<AsyncWebHandler>>, so ~AsyncWebServer() deletes every
// handler it was given. This socket must therefore live on the heap. It used to be
// a file-static object, which made stopWebUi() free() a .bss address — the device
// panicked with "free() target pointer is outside heap areas" and rebooted every
// time a WiFi attack tore the WebUI down (evilportal/karma/deauth/sniffer).
// We keep a borrowed pointer purely to push frames; endWsServer() drops it before
// the server is destroyed.
static AsyncWebSocket *ws = nullptr;
static uint32_t wsEventId = 0;
static String deviceState = "idle";

void beginWsServer(AsyncWebServer *server) {
    if (!server) return;
    ws = new AsyncWebSocket("/ws");
    ws->onEvent([](AsyncWebSocket *srv, AsyncWebSocketClient *cli, AwsEventType type,
                 void *arg, uint8_t *data, size_t len) {
        if (type == WS_EVT_CONNECT) {
            cli->text(String("{\"id\":") + wsEventId + ",\"type\":\"state\",\"device_state\":\"" + deviceState + "\"}");
        }
        // ponytail: client subscribe ignored, app re-fetches /systeminfo for resume
    });
    server->addHandler(ws); // server owns *ws from here on
}

void endWsServer() {
    // Must run BEFORE ~AsyncWebServer(), which deletes the socket.
    AsyncWebSocket *sock = ws;
    ws = nullptr; // pushWsEvent() must stop touching it immediately
    if (!sock) return;

    sock->enable(false); // refuse new handshakes and frames
    sock->closeAll();    // politely close every live client

    // Draining here is not optional. The AsyncTCP task services the disconnects
    // (AsyncClient::_error -> _onDisconnect -> _handleDisconnect). If the server
    // destructor frees this socket while that task is mid-teardown, the client
    // list nodes are freed twice and the device panics with
    // "assert failed: multi_heap_free (head != NULL)". ~AsyncWebSocket() takes no
    // lock, so waiting for the client list to empty is the only guard we have.
    for (int i = 0; i < 100 && sock->count() > 0; ++i) {
        sock->cleanupClients(0); // close + reap; recursive mutex makes this safe
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelay(pdMS_TO_TICKS(200)); // let any in-flight async event retire
}

void pushWsEvent(const String &type, const String &jsonPayload) {
    // The ID is allocated for every event, whether or not anyone is listening.
    // The app resumes with EventStream.lastEventId, which assumes a single
    // monotonic, gap-free ID space that survives WebUI teardown. Allocating only
    // when a WebSocket client happens to be attached stalled the counter, so
    // events raised while the socket was down silently reused IDs the app had
    // already seen and were treated as replays.
    uint32_t id = ++wsEventId;
    if (!ws || ws->count() == 0) return;
    String frame = "{\"id\":" + String(id) + ",\"type\":\"" + type + "\"" + jsonPayload + "}";
    ws->textAll(frame);
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
