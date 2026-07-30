#ifndef __CRASH_COMMANDS_H__
#define __CRASH_COMMANDS_H__

#include <SimpleCLI.h>

void createCrashCommands(SimpleCLI *cli);

// Emits one log_e line naming this boot's reset reason, and whether a stored core
// dump is waiting. log_e, not Serial: UART0 is the only console actually attached
// on this board (ISSUE-22).
void reportBootCrashState();

#endif
