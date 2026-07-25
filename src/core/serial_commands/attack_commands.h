#pragma once
#if !defined(LITE_VERSION)
#include <Arduino.h>
class SimpleCLI;
void createAttackCommands(SimpleCLI *cli);
#endif
