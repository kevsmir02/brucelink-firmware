#pragma once
#if !defined(LITE_VERSION)
#include <Arduino.h>
class AsyncWebServer;

// A transport that wants a copy of every event frame registers here. Kept as a
// plain function pointer so ws_events takes no link dependency on the BLE
// module, which is compiled out entirely under LITE_VERSION.
using EventSink = void (*)(const String &frame);
void registerEventSink(EventSink sink);

void beginWsServer(AsyncWebServer *server);
void endWsServer();
void pushWsEvent(const String &type, const String &jsonPayload);
void pushWsLog(const String &line, const char *level = "info");
void setDeviceState(const String &state);
String getDeviceState();
#endif
