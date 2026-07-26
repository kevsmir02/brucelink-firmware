#pragma once
#if !defined(LITE_VERSION)
#include <Arduino.h>
class AsyncWebServer;
void beginWsServer(AsyncWebServer *server);
void endWsServer();
void pushWsEvent(const String &type, const String &jsonPayload);
void pushWsLog(const String &line, const char *level = "info");
void setDeviceState(const String &state);
String getDeviceState();
#endif
