
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <typeinfo>

extern AsyncWebServer *server; // used to check if the webserver is running

// function defaults
String humanReadableSize(uint64_t bytes);
String listFiles(FS &fs, const String &folder);
String readLineFromFile(File myFile);

void loopOptionsWebUi();

void serveWebUIFile(AsyncWebServerRequest *request, const String &filename, const char *contentType);
void serveWebUIFile(
    AsyncWebServerRequest *request, const String &filename, const char *contentType, bool gzip,
    const uint8_t *originaFile, uint32_t originalFileSize
);
void configureWebServer();
// background = true starts the server and returns immediately, instead of
// holding the screen until ESC. Same end state as picking "Run in background"
// from the on-screen menu; lets a remote client re-arm the WebUI it stopped.
bool startWebUi(bool mode_ap = false, bool background = false);
void stopWebUi();
void cleanlyStopWebUiForWiFiFeature();
