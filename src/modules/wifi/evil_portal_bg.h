#ifndef __EVIL_PORTAL_BG_H__
#define __EVIL_PORTAL_BG_H__

#include <Arduino.h>

// A portal that does not hold the serial task. The blocking EvilPortal runs its
// own while(true) from inside the CLI callback, so while it is up no BLE command
// is parsed at all and, with the WebUI torn down on entry, the device has no
// remote control surface left.

bool evilPortalBgStart(const String &ssid, uint8_t channel, const String &templateFile, uint32_t maxSeconds);
bool evilPortalBgStop();
bool evilPortalBgRunning();
String evilPortalBgStatus();

// Pumped from the serial command task; see the call site in serialcmds.cpp.
void evilPortalBgTick();

#endif
