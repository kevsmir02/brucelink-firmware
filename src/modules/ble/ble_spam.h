#pragma once

#include <Arduino.h>
#include <NimBLEBeacon.h>
#include <NimBLEDevice.h>
#include <NimBLEServer.h>
#include <NimBLEUtils.h>
void spamMenu();

void ibeacon(
    const char *DeviceName = "Bruce iBeacon", const char *BEACON_UUID = "8ec76ea3-6668-48da-9866-75be8bc86f4d",
    int ManufacturerId = 0x4C00
);

// Non-interactive spam entry point for the /cm serial+web command (and thus the
// companion app). Runs a specific spam type for `count` advertisement packets
// without the on-device spamMenu() UI, reporting progress over the /ws stream.
// `name` is one of: apple, android, ibeacon, samsung, windows, random.
// Returns false if the name is unknown.
bool bleSpamRunAttackByName(const String &name, int count);

// True if `name` is a spam type bleSpamRunAttackByName() understands. Lets callers
// validate the verb before committing to radio/AP teardown.
bool bleSpamIsKnownAttackName(const String &name);
