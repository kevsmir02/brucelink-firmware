#if !defined(LITE_VERSION)
#ifndef REVERSE_SHELL_H
#define REVERSE_SHELL_H

#include "core/display.h"

// WPA2 rejects anything shorter than 8 characters, which is what made the previous
// "bruce" unusable. Kept as a macro so the on-screen text cannot drift from the
// passphrase actually handed to softAP().
#define REVERSE_SHELL_AP_PASSWORD "bruceshell"

// Returns false when the AP could not be brought up, so the CLI can report a real
// outcome instead of an unconditional success (ISSUE-7).
bool ReverseShell();

#endif // REVERSE_SHELL_H
#endif // LITE_VERSION
