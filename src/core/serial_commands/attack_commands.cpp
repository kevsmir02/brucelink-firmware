#if !defined(LITE_VERSION)
#include "attack_commands.h"
#include "core/settings.h"
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

void createAttackCommands(SimpleCLI *cli) {
    Command ble = cli->addCompositeCmd("ble");
    Command bleApi = ble.addCommand("api", bleApiCmdCallback);
    bleApi.addPosArg("state", "on");
}
#endif
