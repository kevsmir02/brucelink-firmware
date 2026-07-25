#if !defined(LITE_VERSION)
#include "attack_commands.h"
#include "core/settings.h"
#include "modules/wifi/evil_portal.h"
#include <SimpleCLI.h>
#include <globals.h>

uint32_t bleApiCmdCallback(cmd *c) {
    Command cmd(c);
    String state = cmd.getArgument("state").getValue();
    state.trim();
    if (state == "on") {
        if (!bleApiIsEnabled()) enableBLEAPI();
        serialDevice->println(String("BLE API ") + (bleApiIsEnabled() ? "ON" : "OFF"));
        return bleApiIsEnabled();
    } else if (state == "off") {
        if (bleApiIsEnabled()) enableBLEAPI();
        serialDevice->println(String("BLE API ") + (bleApiIsEnabled() ? "ON" : "OFF"));
        return !bleApiIsEnabled();
    }
    serialDevice->println("usage: ble api on|off");
    return false;
}

uint32_t evilportalCmdCallback(cmd *c) {
    Command cmd(c);
    String ssid = cmd.getArgument("ssid").getValue();
    String chStr = cmd.getArgument("channel").getValue();
    String templateFile = cmd.getArgument("template").getValue();
    ssid.trim();
    chStr.trim();
    templateFile.trim();
    if (ssid.isEmpty()) ssid = "Free Wifi";
    uint8_t channel = (uint8_t)chStr.toInt();
    if (channel < 1 || channel > 13) channel = 6;
    EvilPortal(ssid, channel, false, false, true, false, templateFile);
    return true;
}

void createAttackCommands(SimpleCLI *cli) {
    Command ble = cli->addCompositeCmd("ble");
    Command bleApi = ble.addCommand("api", bleApiCmdCallback);
    bleApi.addPosArg("state", "on");

    Command evilportal = cli->addCommand("evilportal", evilportalCmdCallback);
    evilportal.addPosArg("ssid", "Free Wifi");
    evilportal.addPosArg("channel", "6");
    evilportal.addPosArg("template", "");
}
#endif
