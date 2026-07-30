#include "webInterface.h"
#include "core/display.h"    // using displayRedStripe as error msg
#include "core/mykeyboard.h" // using keyboard when calling rename
#include "core/passwords.h"
#include "core/ram_profile.h"
#include "core/sd_functions.h" // using sd functions called to rename and manage sd files
#include "core/serialcmds.h"
#include "core/settings.h"
#include "core/system_info.h"
#include "core/utils.h"
#include "core/wifi/wifi_common.h" // using common wifisetup
#include "core/wifi/ws_events.h"
#include "core/radio_mem.h"
#include "core/wifi/webui_gate.h"
#include "esp_task_wdt.h"
#include "webFiles.h"
#include <MD5Builder.h>
#include <ArduinoJson.h>
#include <cstddef>
#include <esp32-hal-psram.h>
#include <esp_heap_caps.h>
#include <globals.h>

File uploadFile;
FS _webFS = LittleFS;
// WiFi as a Client
const int default_webserverporthttp = 80;

// WiFi as an Access Point
IPAddress AP_GATEWAY(172, 0, 0, 1); // Gateway

AsyncWebServer *server = nullptr; // initialise webserver
const char *host = "bruce";
String uploadFolder = "";
static bool mdnsRunning = false;

// Generate random token
String generateToken(int length = 24) {
    String token = "";
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    for (int i = 0; i < length; i++) { token += charset[random(0, sizeof(charset) - 1)]; }
    return token;
}

/**********************************************************************
**  Function: stopWebUi
**  Turn off the WebUI
**********************************************************************/
void stopWebUi() {
    tft.setLogging(false);
    isWebUIActive = false;
    endWsServer(); // drop our borrowed /ws pointer; the server dtor below frees it
    server->end();
    server->~AsyncWebServer();
    free(server);
    server = nullptr;
    if (mdnsRunning) {
        MDNS.end();
        mdnsRunning = false;
    }
}

/**********************************************************************
**  Function: cleanlyStopWebUiForWiFiFeature
**  Cleanly stop WebUI and AP mode before starting a WiFi feature
**  This prevents WiFi mode conflicts when features need exclusive control
**********************************************************************/
void cleanlyStopWebUiForWiFiFeature() {
    // Only proceed if WebUI is active
    if (!isWebUIActive && !server) { return; }

    // Brief notification (non-blocking)
    Serial.println("Stopping WebUI for WiFi feature...");

    // Stop the WebUI
    if (server) {
        stopWebUi();
        // Give the web server time to fully shut down
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // Disconnect WiFi AP mode if it's the WebUI's AP
    // Check if we're in AP or APSTA mode (used by WebUI)
    wifi_mode_t currentMode = WiFi.getMode();
    if (currentMode == WIFI_MODE_AP || currentMode == WIFI_MODE_APSTA) {
        wifiDisconnect();
        // Give WiFi time to fully disconnect
        vTaskDelay(pdMS_TO_TICKS(250));
    }

    Serial.println("WebUI stopped, starting WiFi feature...");
}
/**********************************************************************
**  Function: loopOptionsWebUi
**  Display options to launch the WebUI
**********************************************************************/
void loopOptionsWebUi() {
    if (isWebUIActive) {
        bool opt = WiFi.getMode() - 1;
        options = {
            {"Stop WebUI", stopWebUi},
            {"WebUi screen", lambdaHelper(startWebUi, opt, false, false)}
        };
        addOptionToMainMenu();
        loopOptions(options);
        return;
    }
    options = {
        {"my Network", lambdaHelper(startWebUi, false, false, false)},
        {"AP mode",    lambdaHelper(startWebUi, true, false, false)},
    };

    loopOptions(options);
    // On fail installing will run the following line
}

/**********************************************************************
**  Function: humanReadableSize
** Make size of files human readable
** source: https://github.com/CelliesProjects/minimalUploadAuthESP32
**********************************************************************/
String humanReadableSize(uint64_t bytes) {
    if (bytes < 1024) return String(bytes) + " B";
    else if (bytes < (1024 * 1024)) return String(bytes / 1024.0) + " kB";
    else if (bytes < (1024 * 1024 * 1024)) return String(bytes / 1024.0 / 1024.0) + " MB";
    else return String(bytes / 1024.0 / 1024.0 / 1024.0) + " GB";
}

/**********************************************************************
**  Function: listFiles
**  list all of the files, if ishtml=true, return html rather than simple text
**********************************************************************/
String listFiles(FS &fs, const String &folder) {
    // log_i("Listfiles Start");
    String returnText = "pa:" + folder + ":0\n";
    // Serial.println("Listing files stored on SD");

    _webFS = fs;

    File root = fs.open(folder);
    uploadFolder = folder;

    while (true) {
        bool isDir;
        String fullPath = root.getNextFileName(&isDir);
        String nameOnly = fullPath.substring(fullPath.lastIndexOf("/") + 1);
        if (fullPath == "") { break; }
        // Serial.printf("Path: %s (isDir: %d)\n", fullPath.c_str(), isDir);

        if (esp_get_free_heap_size() > (String("Fo:" + nameOnly + ":0\n").length()) + 1024) {
            if (isDir) {
                // Serial.printf("Directory: %s\n", fullPath.c_str());
                returnText += "Fo:" + nameOnly + ":0\n";
            } else {
                // For files, we need to get the size, so we open the file briefly
                // Serial.printf("Opening file for size check: %s\n", fullPath.c_str());
                File file = fs.open(fullPath);
                // Serial.printf("File size: %llu bytes\n", file.size());
                if (file) {
                    returnText += "Fi:" + nameOnly + ":" + humanReadableSize(file.size()) + "\n";
                    file.close();
                }
            }
        } else break;
        delay(1);
    }
    root.close();
    // log_i("ListFiles End");
    return returnText;
}

/**********************************************************************
**  Function: checkUserWebAuth
** used by server->on functions to discern whether a user has the correct
** httpapitoken OR is authenticated by username and password
**********************************************************************/
bool checkUserWebAuth(AsyncWebServerRequest *request, bool onFailureReturnLoginPage = false) {
    // Authorization: Bearer <token> (mobile companion convenience)
    if (request->hasHeader("Authorization")) {
        const AsyncWebHeader *auth = request->getHeader("Authorization");
        String v = auth->value();
        const char *prefix = "Bearer ";
        if (v.startsWith(prefix)) {
            String token = v.substring(strlen(prefix));
            token.trim();
            if (bruceConfig.isValidWebUISession(token)) return true;
        }
    }
    if (request->hasHeader("Cookie")) {
        const AsyncWebHeader *cookie = request->getHeader("Cookie");
        String c = cookie->value();
        int idx = c.indexOf("BRUCESESSION=");
        if (idx != -1) {
            int start = idx + 13;
            int end = c.indexOf(';', start);
            if (end == -1) end = c.length();
            String token = c.substring(start, end);
            if (bruceConfig.isValidWebUISession(token)) { return true; }
        }
    }
    if (onFailureReturnLoginPage) {
        serveWebUIFile(request, "login.html", "text/html", true, login_html, login_html_size);
    } else {
        request->send(401, "text/plain", "Unauthorized");
    }
    return false;
}

/**********************************************************************
**  Function: createDirRecursive
** Create folders recursivelly
**********************************************************************/
void createDirRecursive(const String &path, FS fs) {
    String currentPath = "";
    int startIndex = 0;
    // Serial.print("Verifying folder: ");
    // Serial.println(path);

    while (startIndex < path.length()) {
        int endIndex = path.indexOf("/", startIndex);
        if (endIndex == -1) endIndex = path.length();

        currentPath += path.substring(startIndex, endIndex);
        if (currentPath.length() > 0) {
            if (!fs.exists(currentPath)) {
                fs.mkdir(currentPath);
                // Serial.print("Creating folder: ");
                // Serial.println(currentPath);
            }
        }

        if (endIndex < path.length()) { currentPath += "/"; }
        startIndex = endIndex + 1;
    }
}
/**********************************************************************
**  Function: handleUpload
** handles uploads to the filserver
**********************************************************************/
void handleUpload(
    AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final
) {
    if (checkUserWebAuth(request)) {
        if (uploadFolder == "/") uploadFolder = "";
        if (!index) {
            if (request->hasArg("password")) filename = filename + ".enc";
            // Serial.println("File: " + uploadFolder + "/" + filename);
            String relativePath = filename;
            String fullPath = uploadFolder + "/" + relativePath;
            String dirPath = fullPath.substring(0, fullPath.lastIndexOf("/"));
            if (dirPath.length() > 0) { createDirRecursive(dirPath, _webFS); }
        RETRY:
            request->_tempFile = _webFS.open(uploadFolder + "/" + filename, "w");
            if (!request->_tempFile) {
                // Serial.println("Failed to open file for writing: " + uploadFolder + "/" + filename);
                goto RETRY;
            }
        }

        if (len) {
            if (request->hasArg("password")) {
                // encryption requested
                static int chunck_no = 0;
                if (chunck_no != 0) {
                    // TODO: handle multiple chunks
                    request->send(404, "text/html", "file is too big");
                    return;
                } else chunck_no += 1;
                String enc_password = request->arg("password");
                String plaintext = String((char *)data).substring(0, len);
                String cyphertxt = encryptString(plaintext, enc_password);
                if (cyphertxt == "") { return; }
                if (request->_tempFile)
                    request->_tempFile.write((const uint8_t *)cyphertxt.c_str(), cyphertxt.length());
            } else {
                if (request->_tempFile) request->_tempFile.write(data, len);
            }
        }
        if (final) {
            // close the file handle as the upload is now done
            if (request->_tempFile) request->_tempFile.close();
        }
    }
}

void notFound(AsyncWebServerRequest *request) { request->send(404, "text/plain", "Nothing in here Sharky"); }

/**********************************************************************
**  Function: drawWebUiScreen
**  Draw information on screen of WebUI.
**********************************************************************/
void drawWebUiScreen(bool mode_ap) {
    drawMainBorderWithTitle("WebUI", true);

    String txt;
    if (!mode_ap) txt = WiFi.localIP().toString();
    else txt = WiFi.softAPIP().toString();

    int padX = 14;
    int currentY = 55;

    tft.setTextColor(bruceConfig.priColor, bruceConfig.bgColor);
    tft.setTextSize(FP);

    if (mode_ap) {
        tft.setCursor(padX, currentY);
        tft.print("Net: BruceNet/brucenet");
        currentY += LH * FP + 6;
    }

    tft.setCursor(padX, currentY);
    if (mdnsRunning) tft.print("Url: http://bruce.local");
    currentY += LH * FP + 6;

    tft.setCursor(padX, currentY);
    tft.print("IP:  " + txt);
    currentY += LH * FP + 6;

    tft.setCursor(padX, currentY);
    tft.print("Usr: " + String(bruceConfig.webUI.user));
    currentY += LH * FP + 6;

    tft.setCursor(padX, currentY);
    tft.print("Pwd: " + String(bruceConfig.webUI.pwd));

    tft.setTextColor(TFT_RED, bruceConfig.bgColor);
    tft.setTextSize(FP);
    tft.drawCentreString("press Esc to stop", tftWidth / 2, tftHeight - 2 * LH * FP - 5, 1);

#if defined(HAS_TOUCH)
    TouchFooter();
#endif
}

/**********************************************************************
**  Function: color565ToWebHex
**  convert 565 color to web hex format for theme purposes
**********************************************************************/
String color565ToWebHex(uint16_t color565) {
    // Extract RGB components from 565
    uint8_t r = (color565 >> 11) & 0x1F;
    uint8_t g = (color565 >> 5) & 0x3F;
    uint8_t b = color565 & 0x1F;

    // Scale up to 8 bits
    r = (r << 3) | (r >> 2);
    g = (g << 2) | (g >> 4);
    b = (b << 3) | (b >> 2);

    char hex[8];
    snprintf(hex, sizeof(hex), "#%02X%02X%02X", r, g, b);
    return String(hex);
}

/**********************************************************************
**  Function: serveWebUIFile
**  serves files for WebUI and checks for custom WebUI files
**********************************************************************/
void serveWebUIFile(AsyncWebServerRequest *request, const String &filename, const char *contentType) {
    serveWebUIFile(request, filename, contentType, false, nullptr, 0);
}
void serveWebUIFile(
    AsyncWebServerRequest *request, const String &filename, const char *contentType, bool gzip,
    const uint8_t *originaFile, uint32_t originalFileSize
) {
    AsyncWebServerResponse *response = nullptr;
    FS *fs = NULL;
    if (setupSdCard()) {
        if (SD.exists("/BruceWebUI/" + filename)) fs = &SD;
    } else if (LittleFS.exists("/BruceWebUI/" + filename)) {
        fs = &LittleFS;
    }
    if (fs) {
        response = request->beginResponse(*fs, "/BruceWebUI/" + filename, contentType);
    } else {
        if (filename == "theme.css") {
            String css = ":root{--color:" + color565ToWebHex(bruceConfig.priColor) +
                         ";--sec-color:" + color565ToWebHex(bruceConfig.secColor) +
                         ";--background:" + color565ToWebHex(bruceConfig.bgColor) + ";}";
            AsyncWebServerResponse *themeResponse = request->beginResponse(200, "text/css", css);
            request->send(themeResponse);
            return;
        }
        response = request->beginResponse(200, String(contentType), originaFile, originalFileSize);
        if (gzip) {
            if (!response->addHeader("Content-Encoding", "gzip")) log_e("Failed to add gzip header");
        }
    }
    request->send(response);
}

/**********************************************************************
**  Function: startMdnsResponder
**  Try to start mDNS only if there is enough internal heap available
**********************************************************************/
static bool startMdnsResponder() {
    RAM_LOG("before MDNS");

    if (!MDNS.begin(host)) {
        RAM_LOG("MDNS failed");
        Serial.printf("Error setting up MDNS responder!\n");
        return false;
    }

    RAM_LOG("after MDNS");
    return true;
}

/**********************************************************************
**  Function: configureWebServer
**  configure web server
**********************************************************************/
void configureWebServer() {
    mdnsRunning = startMdnsResponder();
    DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
    server->onNotFound(notFound);

    // Index
    server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (checkUserWebAuth(request, true)) {
            serveWebUIFile(request, "index.html", "text/html", true, index_html, index_html_size);
        }
    });

    // Login
    server->on("/login", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (request->hasParam("username", true) && request->hasParam("password", true)) {
            String username = request->getParam("username", true)->value();
            String password = request->getParam("password", true)->value();

            if (username == bruceConfig.webUI.user && password == bruceConfig.webUI.pwd) {
                String token = generateToken();
                AsyncWebServerResponse *response = request->beginResponse(302);
                response->addHeader("Location", "/");
                response->addHeader("Set-Cookie", "BRUCESESSION=" + token + "; Path=/; HttpOnly");
                request->send(response);
                bruceConfig.addWebUISession(token);
                return;
            }
        }
        AsyncWebServerResponse *response = request->beginResponse(302);
        response->addHeader("Location", "/?failed");
        request->send(response);
    });

    // Logout
    server->on("/logout", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (request->hasHeader("Cookie")) {
            const AsyncWebHeader *cookie = request->getHeader("Cookie");
            String c = cookie->value();
            int idx = c.indexOf("BRUCESESSION=");
            if (idx != -1) {
                int start = idx + 13;
                int end = c.indexOf(';', start);
                if (end == -1) end = c.length();
                String token = c.substring(start, end);
                bruceConfig.removeWebUISession(token);
            }
        }
        AsyncWebServerResponse *response = request->beginResponse(302);
        response->addHeader("Location", "/?loggedout");
        response->addHeader("Set-Cookie", "BRUCESESSION=0; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT");
        request->send(response);
    });

    // Static files
    server->on("/theme.css", HTTP_GET, [](AsyncWebServerRequest *request) {
        serveWebUIFile(request, "theme.css", "text/css");
    });
    server->on("/index.css", HTTP_GET, [](AsyncWebServerRequest *request) {
        serveWebUIFile(request, "index.css", "text/css", true, index_css, index_css_size);
    });
    server->on("/index.js", HTTP_GET, [](AsyncWebServerRequest *request) {
        serveWebUIFile(request, "index.js", "text/javascript", true, index_js, index_js_size);
    });

    // System Info
    server->on("/systeminfo", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (!checkUserWebAuth(request)) return;
        request->send(200, "application/json", buildSystemInfoJson());
    });

    // Get Screen
    server->on("/getscreen", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (checkUserWebAuth(request)) {
            static uint8_t *screenBinBuffer = nullptr;
            static size_t screenBinBufferSize = 0;

            if (!screenBinBuffer) {
                size_t desiredSize = MAX_LOG_ENTRIES * MAX_LOG_SIZE;
                if (psramFound()) screenBinBuffer = static_cast<uint8_t *>(ps_malloc(desiredSize));
                if (!screenBinBuffer) screenBinBuffer = static_cast<uint8_t *>(malloc(desiredSize));
                if (!screenBinBuffer) {
                    request->send(503, "text/plain", "Insufficient memory for screen buffer");
                    return;
                }
                screenBinBufferSize = desiredSize;
            }

            size_t binSize = 0;
            tft.getBinLog(screenBinBuffer, binSize);
            if (binSize > screenBinBufferSize) {
                request->send(500, "text/plain", "Screen buffer overflow");
                return;
            }
            request->send(200, "application/octet-stream", (const uint8_t *)screenBinBuffer, binSize);
        }
    });

    // Rename file or folder
    server->on("/rename", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (checkUserWebAuth(request)) {
            if (request->hasArg("fileName") && request->hasArg("filePath")) {
                String fs = request->arg("fs").c_str();
                String fileName = request->arg("fileName").c_str();
                String filePath = request->arg("filePath").c_str();
                String filePath2 = filePath.substring(0, filePath.lastIndexOf('/') + 1) + fileName;
                // Rename the file of folder
                if (fs == "SD") {
                    if (SD.rename(filePath, filePath2))
                        request->send(200, "text/plain", filePath + " renamed to " + filePath2);
                    else request->send(200, "text/plain", "Fail renaming file.");
                } else {
                    if (LittleFS.rename(filePath, filePath2))
                        request->send(200, "text/plain", filePath + " renamed to " + filePath2);
                    else request->send(200, "text/plain", "Fail renaming file.");
                }
            }
        }
    });

    // Route to send a generic command (Tasmota compatible API)
    // https://tasmota.github.io/docs/Commands/#with-web-requests
    server->on("/cm", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (!checkUserWebAuth(request)) { return; }
        if (request->hasArg("cmnd")) {
            String cmnd = request->arg("cmnd");
            if (cmnd.startsWith("nav")) {
                volatile bool *var = &SelPress;
                if (cmnd.startsWith("nav sel")) var = &SelPress;
                if (cmnd.startsWith("nav esc")) var = &EscPress;
                if (cmnd.startsWith("nav up")) var = &UpPress;
                if (cmnd.startsWith("nav down")) var = &DownPress;
                if (cmnd.startsWith("nav next")) var = &NextPress;
                if (cmnd.startsWith("nav prev")) var = &PrevPress;
                request->send(200, "text/plain", "command " + cmnd + " success");
                int time;
                if (cmnd.endsWith("0")) time = cmnd.substring(cmnd.lastIndexOf(' ')).toInt();
                else time = 10;
                auto tmp = millis() + time;
                while (tmp > millis()) {
                    AnyKeyPress = true;
                    SerialCmdPress = true;
                    *var = true;
                    if (!LongPress) vTaskDelay(pdMS_TO_TICKS(190));
                    else vTaskDelay(pdMS_TO_TICKS(50));
                }
                // Release the button, which nothing else does. check() clears a flag as
                // it reads it, but the main menu never reads EscPress — its check is
                // gated on menuType != MENU_TYPE_MAIN (display.cpp) — so a `nav esc`
                // delivered there used to latch true forever and then fire inside
                // whatever menu the operator opened next (ISSUE-29). Leaving the flag
                // high also denies the release edge that ScrollableTextArea waits for
                // before it will accept a press, which is why a single pulse could not
                // release a blocked verb (ISSUE-19).
                *var = false;
                AnyKeyPress = false;
                SerialCmdPress = false;
            } else {
                if (parseSerialCommand(cmnd, false)) {
                    request->send(200, "text/plain", "command " + cmnd + " queued");
                } else {
                    request->send(400, "text/plain", "command failed, check the serial log for details");
                }
            }
        } else {
            request->send(400, "text/plain", "http request missing required arg: cmnd");
        }
    });

    // Reboot device
    server->on("/reboot", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (checkUserWebAuth(request)) { ESP.restart(); }
    });

    // List files
    server->on("/listfiles", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (checkUserWebAuth(request)) {
            String folder = "/";
            if (request->hasArg("folder")) { folder = request->arg("folder"); }
            if (strcmp(request->arg("fs").c_str(), "SD") == 0) {
                request->send(200, "text/plain", listFiles(SD, folder));
            } else {
                request->send(200, "text/plain", listFiles(LittleFS, folder));
            }
        }
    });

    // Download, create folder and delete
    server->on("/file", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (checkUserWebAuth(request)) {
            if (request->hasArg("name") && request->hasArg("action")) {
                String fileName = request->arg("name").c_str();
                String fileAction = request->arg("action").c_str();
                String fileSys = request->arg("fs").c_str();
                bool useSD = false;
                if (fileSys == "SD") useSD = true;

                FS *fs;
                if (useSD) {
                    fs = &SD;
                } else fs = &LittleFS;

                log_i("filename: %s\n", fileName.c_str());
                log_i("fileAction: %s\n", fileAction.c_str());

                if (!fs->exists(fileName)) {
                    if (strcmp(fileAction.c_str(), "create") == 0) {
                        if (fs->mkdir(fileName)) {
                            request->send(200, "text/plain", "Created new folder: " + String(fileName));
                        } else {
                            request->send(200, "text/plain", "FAIL creating folder: " + String(fileName));
                        }
                    } else if (strcmp(fileAction.c_str(), "createfile") == 0) {
                        File newFile = fs->open(fileName, FILE_WRITE, true);
                        if (newFile) {
                            newFile.close();
                            request->send(200, "text/plain", "Created new file: " + String(fileName));
                        } else {
                            request->send(200, "text/plain", "FAIL creating file: " + String(fileName));
                        }
                    } else request->send(400, "text/plain", "ERROR: file does not exist");

                } else {
                    if (strcmp(fileAction.c_str(), "download") == 0) {
                        request->send(*fs, fileName, "application/octet-stream", true);
                    } else if (strcmp(fileAction.c_str(), "image") == 0) {
                        String extension = fileName.substring(fileName.lastIndexOf('.') + 1);
                        // https://www.iana.org/assignments/media-types/media-types.xhtml#image
                        if (extension == "jpg") extension = "jpeg"; // www.rfc-editor.org/rfc/rfc2046.html
                        request->send(*fs, fileName, "image/" + extension);
                    } else if (strcmp(fileAction.c_str(), "delete") == 0) {
                        if (deleteFromSd(*fs, fileName)) {
                            request->send(200, "text/plain", "Deleted : " + String(fileName));
                        } else {
                            request->send(200, "text/plain", "FAIL deleting: " + String(fileName));
                        }
                    } else if (strcmp(fileAction.c_str(), "create") == 0) {
                        if (fs->mkdir(fileName)) {
                            request->send(200, "text/plain", "Created new folder: " + String(fileName));
                        } else {
                            request->send(200, "text/plain", "FAIL creating folder: " + String(fileName));
                        }
                    } else if (strcmp(fileAction.c_str(), "createfile") == 0) {
                        File newFile = fs->open(fileName, FILE_WRITE, true);
                        if (newFile) {
                            newFile.close();
                            request->send(200, "text/plain", "Created new file: " + String(fileName));
                        } else {
                            request->send(200, "text/plain", "FAIL creating file: " + String(fileName));
                        }

                    } else if (strcmp(fileAction.c_str(), "edit") == 0) {
                        File editFile = fs->open(fileName, FILE_READ);
                        if (editFile) {
                            String fileContent = editFile.readString();
                            request->send(200, "text/plain", fileContent);
                            editFile.close();
                        } else {
                            request->send(500, "text/plain", "Failed to open file for reading");
                        }

                    } else {
                        request->send(400, "text/plain", "ERROR: invalid action param supplied");
                    }
                }
            } else {
                request->send(400, "text/plain", "ERROR: name and action params required");
            }
        }
    });

    // Edit file
    server->on("/edit", HTTP_POST, [](AsyncWebServerRequest *request) {
        if (checkUserWebAuth(request)) {
            if (request->hasArg("name") && request->hasArg("content") && request->hasArg("fs")) {
                String fileName = request->arg("name");
                String fileContent = request->arg("content");
                bool useSD = false;

                if (strcmp(request->arg("fs").c_str(), "SD") == 0) { useSD = true; }

                fs::FS *fs = useSD ? (fs::FS *)&SD : (fs::FS *)&LittleFS;
                String fsType = useSD ? "SD" : "LittleFS";

                if (useSD) {              // LittleFS is already mounted
                    if (!setupSdCard()) { // only tries to mount SD if editting on SD
                        request->send(500, "text/plain", "Failed to initialize file system: " + fsType);
                        return;
                    }
                }

                File editFile = fs->open(fileName, FILE_WRITE);
                if (editFile) {
                    if (editFile.write((const uint8_t *)fileContent.c_str(), fileContent.length())) {
                        request->send(200, "text/plain", "File edited: " + fileName);
                    } else {
                        request->send(500, "text/plain", "Failed to write to file: " + fileName);
                    }
                    editFile.close();
                } else {
                    request->send(500, "text/plain", "Failed to open file for writing: " + fileName);
                }

            } else {
                request->send(400, "text/plain", "ERROR: name, content, and fs parameters required");
            }
        }
    });

    // File upload
    server->on(
        "/upload",
        HTTP_POST,
        [](AsyncWebServerRequest *request) { request->send(200, "text/plain", "File upload completed"); },
        handleUpload
    );

    // Wi-Fi configuration
    server->on("/wifi", HTTP_GET, [](AsyncWebServerRequest *request) {
        if (checkUserWebAuth(request)) {
            if (request->hasArg("usr") && request->hasArg("pwd")) {
                const char *usr = request->arg("usr").c_str();
                const char *pwd = request->arg("pwd").c_str();
                bruceConfig.setWebUICreds(usr, pwd);
                request->send(
                    200, "text/plain", "User: " + String(usr) + " configured with password: " + String(pwd)
                );
            }
        }
    });

    RAM_LOG("webui pre-ws");
    beginWsServer(server);
    server->begin();
    RAM_LOG("webui post-begin");
    Serial.println("Webserver started");
}

// Every WebUI start outcome goes through here. Four destinations because no single
// one reaches everybody on this board: `Serial` is the native USB-CDC port that
// nothing is attached to (ISSUE-22), the CLI reply is invisible to anyone watching
// the console, and the console is invisible to the app.
static void reportWebUiStart(const WebUiStartReport &r) {
    const String line = String(formatWebUiStartReport(r).c_str());
    const bool ok = r.result == WebUiStartResult::Started;

    // log_e for failures because CORE_DEBUG_LEVEL=1 compiles out every level below
    // ERROR; RAM_LOGF for the success line, which reaches the same console via the
    // UART0 mirror without misreporting itself as an error.
    if (ok) RAM_LOGF("%s", line.c_str());
    else log_e("%s", line.c_str());

    pushWsLog(line, ok ? "info" : "err");
    if (serialDevice) serialDevice->println(line);
    // false, never true: waitKeyPress spins on check(AnyKeyPress) and would hold the
    // serial task here with no BLE dismissal available.
    if (!ok) displayError(line, false);
}

/**********************************************************************
**  Function: startWebUi
**  Start the WebUI
**********************************************************************/
bool startWebUi(bool mode_ap, bool background, bool selftest) {
    WebUiStartReport report{};
    report.required = RADIO_WIFI_MIN_DMA_BLOCK;
    report.apMode = mode_ap;

    bool keepWifiConnected = false;
    if (WiFi.status() != WL_CONNECTED) {
        // The return was discarded here, so neither the STA memory gate nor a failed
        // softAP() could reach this caller and the WebUI was built on top of an
        // interface that might not exist.
        if (!wifiConnectMenu(mode_ap ? WIFI_AP : WIFI_STA)) {
            report.result = WebUiStartResult::WifiBringUpFailed;
            report.dmaBlock = radioLargestDmaBlock();
            reportWebUiStart(report);
            return false;
        }
    } else {
        keepWifiConnected = true;
    }

    // configure web server

    if (!server) {
        // Clear this vector to free stack memory
        options.clear();

        Serial.println("Configuring Webserver ...");
        RAM_LOG("webui pre-alloc");

        // Sampled here rather than at entry: this is the point ISSUE-12's
        // `webui pre-alloc` figures were taken, with the AP already up, so the gate
        // is directly comparable to the register's table. At function entry the
        // block is still pre-AP and was ample in both failing runs.
        report.dmaBlock = radioLargestDmaBlock();
        if (!webUiDmaSufficient(report.dmaBlock, report.required)) {
            report.result = WebUiStartResult::RefusedLowDmaPreAlloc;
            reportWebUiStart(report);
            // Refusing must not leave an AP we raised on air: ISSUE-31 and ISSUE-39
            // are both that mistake, and `free` failing to return to its idle
            // plateau was the whole tell.
            if (!keepWifiConnected) wifiDisconnect();
            return false;
        }

        if (psramFound()) server = (AsyncWebServer *)ps_malloc(sizeof(AsyncWebServer));
        else server = (AsyncWebServer *)malloc(sizeof(AsyncWebServer));

        new (server) AsyncWebServer(default_webserverporthttp);

        configureWebServer();
        RAM_LOG("webui post-configure");

        // configureWebServer() ends in server->begin(). A failed begin() returns
        // early leaving _pcb null, so state() reads CLOSED — the one exact signal
        // that port 80 is not listening, as against a heap figure that merely
        // correlates with it.
        // -selftest forces the branch below without stubbing begin(): the server is
        // genuinely started and genuinely unwound, which is the half that has to be
        // proven. ISSUE-28's beginAP() guard is still UNVERIFIED because nothing can
        // reach it from the CLI; this exists so gate D does not join it.
        report.tcpState = selftest ? (uint8_t)0 : (uint8_t)server->state();
        if (!webUiListening(report.tcpState)) {
            report.result = WebUiStartResult::FailedNotListening;
            report.dmaBlock = radioLargestDmaBlock();
            reportWebUiStart(report);
            stopWebUi();
            if (!keepWifiConnected) wifiDisconnect();
            return false;
        }

        // Only now. Set unconditionally before, this told display.cpp:985 and
        // loopOptionsWebUi() that a server existed when none did.
        isWebUIActive = true;
    }

    report.result = WebUiStartResult::Started;
    report.dmaBlock = radioLargestDmaBlock();
    report.tcpState = (uint8_t)server->state();
    reportWebUiStart(report);
    tft.setLogging();
    drawWebUiScreen(mode_ap);
#ifdef HAS_SCREEN // Headless always run in the background!
    if (background) return true;
    while (!check(EscPress)) {
        // nothing here, just to hold the screen until the server is on.
        vTaskDelay(pdMS_TO_TICKS(70));
    }

    bool closeServer = false;

    options.clear();
    options.emplace_back("Run in background", []() {});
    options.emplace_back("Exit", [&closeServer]() { closeServer = true; });

    loopOptions(options);

    if (closeServer) {
        stopWebUi();
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!keepWifiConnected) { wifiDisconnect(); }
    }
#endif
    return true;
}
