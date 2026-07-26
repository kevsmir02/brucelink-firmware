#ifndef __SYSTEM_INFO_H__
#define __SYSTEM_INFO_H__

#include <Arduino.h>

// Serialized system-info JSON, shared by the HTTP /systeminfo handler and the
// `systeminfo` CLI verb so both transports return byte-identical data.
String buildSystemInfoJson();

#endif
