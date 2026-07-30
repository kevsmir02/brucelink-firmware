#include "wifi_commands.h"
#include "core/wifi/webInterface.h"
#include "core/wifi/wifi_common.h" //to return MAC addr
#include <globals.h>
#if !defined(LITE_VERSION)
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "modules/wifi/sniffer.h"
#include "modules/wifi/tcp_utils.h"
#include <modules/ethernet/ARPScanner.h>
// #include "modules/wifi/responder.h"
#endif
uint32_t wifiCallback(cmd *c) {
    Command cmd(c);
    Argument statusArg = cmd.getArgument("status");
    String status = statusArg.getValue();
    status.trim();

    Argument ssidArg = cmd.getArgument("ssid");
    String ssid = ssidArg.getValue();
    ssid.trim();

    Argument pwdArg = cmd.getArgument("pwd");
    String pwd = pwdArg.getValue();
    pwd.trim();

    if (status == "off") {
        wifiDisconnect();
        return true;
    } else if (status == "on") {
        if (wifiConnected) {
            serialDevice->println("Wifi already connected");
            return true;
        }
        if (wifiConnecttoKnownNet()) return true;
        wifiDisconnect();
        return _setupAP();

    } else if (status == "add" && ssid != "" && pwd != "") {
        bruceConfig.addWifiCredential(ssid, pwd);
        return true;
    } else {
        serialDevice->println(
            "Invalid status: " + status +
            "\n"
            "Possible commands: \n"
            "-> wifi off (Disconnects Wifi)\n"
            "-> wifi on  (Connects to a known Wifi network. if there's no known network, starts in AP Mode)\n"
            "-> wifi add SSID Password (adds a network to the list)"
        );
        return false;
    }
}

uint32_t webuiCallback(cmd *c) {
    Command cmd(c);

    // `webui off` frees the WebUI and its AP without touching BLE, so a client
    // that has moved control onto BLE can hand the WiFi memory back. Running
    // both transports at once does not fit on this board: loaded with BLE + AP
    // + WebUI there is ~15 KB free, and a single associated station drives that
    // to a few hundred bytes, at which point notifies truncate and HTTP dies.
    // Reuses the teardown WiFi attacks already use.
    if (cmd.getArgument("off").isSet()) {
        cleanlyStopWebUiForWiFiFeature();
        serialDevice->println("WebUI stopped");
        return true;
    }

    Argument arg = cmd.getArgument("noAp");
    bool noAp = arg.isSet();
    bool background = cmd.getArgument("bg").isSet();
    bool selftest = cmd.getArgument("selftest").isSet();

    // Parenthesised because `+` binds tighter than `?:`: without them the whole
    // concatenation became the ternary's condition and the client received a bare
    // "AP"/"STA" with the prefix silently dropped.
    serialDevice->println(String("Starting Web UI ") + (!noAp ? "AP" : "STA"));
    serialDevice->println("Press ESC to quit");
    // Returned, not discarded: this is the one attack-adjacent verb whose
    // `[CLI] Result:` can now mean something, since startWebUi knows whether port
    // 80 is listening.
    return startWebUi(!noAp, background, selftest); // without bg: quits when check(EscPress)
}
#if !defined(LITE_VERSION)
uint32_t scanHostsCallback(cmd *c) {
    esp_netif_t *esp_netinterface = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (esp_netinterface == nullptr) {
        // serialDevice, not Serial: this is the verb's only failure text, and Serial
        // reaches nothing on this board (ISSUE-22), so over BLE `arp` returned a bare
        // Result: FALSE with no reason.
        serialDevice->println("Failed to get netif handle\nTry connecting to a network first");
        return false;
    }

    ARPScanner{esp_netinterface};

    return true;
}

uint32_t snifferCallback(cmd *c) {
    sniffer_setup();

    return true;
}

uint32_t listenTCPCallback(cmd *c) {
    if (!wifiConnected) {
        serialDevice->println("Connect to a WiFi first.");
        return false;
    }

    listenTcpPort();

    return true;
}
#endif
/*
uint32_t responderCallback(cmd *c) {
    if (!wifiConnected) {
        Serial.println("Connect to a WiFi first.");
        return false;
    }

    responder();

    return true;
}
*/

void createWifiCommands(SimpleCLI *cli) {
    Command webuiCmd = cli->addCommand("webui", webuiCallback);
    webuiCmd.addFlagArg("noAp");
    webuiCmd.addFlagArg("off");
    webuiCmd.addFlagArg("bg");
    webuiCmd.addFlagArg("selftest");

    Command wifiCmd = cli->addCommand("wifi", wifiCallback);
    wifiCmd.addPosArg("status");
    wifiCmd.addPosArg("ssid", "");
    wifiCmd.addPosArg("pwd", "");

#if !defined(LITE_VERSION)

    Command ScanHostsCmd = cli->addCommand("arp", scanHostsCallback);

    Command listenTCPCmd =
        cli->addCommand("listen", listenTCPCallback); // TODO: make possible to select port to open via Serial

    Command snifferCmd =
        cli->addCommand("sniffer", snifferCallback); // TODO: be able to exit from it from Serial

#endif
    // Command responderCmd = cli->addCommand("responder", responderCallback); TODO
}
