#include "crash_commands.h"
#include "core/crash_report.h"
#include <SimpleCLI.h>
#include <esp_app_desc.h>
#include <esp_system.h>
#include <globals.h>
#include <sdkconfig.h>
#include <string.h>

// sdkconfig is included explicitly rather than relied on transitively, because the
// guard below decides whether this whole file has a core dump to read.
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH && CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
#include <esp_core_dump.h>

static bool haveStoredDump() {
    size_t addr = 0, size = 0;
    return esp_core_dump_image_get(&addr, &size) == ESP_OK && size > 0;
}

static uint32_t crashlogCallback(cmd *c) {
    Command cmd(c);

    const int reason = (int)esp_reset_reason();
    serialDevice->printf("crash: reset_reason=%s(%d)\r\n", resetReasonName(reason), reason);

    if (cmd.getArgument("selftest").isSet()) {
        // Deliberate abort, because the crash path cannot otherwise be tested with a
        // known-good answer: every real crash here is a race, so a clean run would
        // prove nothing about whether the dump was written and parsed correctly.
        // Flush by hand -- parse() never returns, so the usual prompt+EOT never runs
        // and a client would otherwise wait out its timeout on a device that died.
        serialDevice->println("crash: aborting deliberately to test the dump path");
        serialDevice->endOfResponse();
        vTaskDelay(pdMS_TO_TICKS(300));
        abort();
    }

    if (cmd.getArgument("clear").isSet()) {
        esp_err_t err = esp_core_dump_image_erase();
        // Both ternary branches are String: mixing a literal with a String would
        // make the common type depend on a user-defined conversion.
        serialDevice->println(
            err == ESP_OK ? String("crash: cleared") : String("crash: erase failed err=") + (int)err
        );
        return err == ESP_OK;
    }

    if (!haveStoredDump()) {
        serialDevice->println("crash: none stored");
        return true;
    }

    esp_core_dump_summary_t summary;
    esp_err_t err = esp_core_dump_get_summary(&summary);
    if (err != ESP_OK) {
        // A dump that exists but will not parse is itself the finding: say so rather
        // than reporting "none stored" and losing the distinction.
        serialDevice->println(String("crash: stored dump unreadable err=") + (int)err);
        return false;
    }

    CrashSummaryView view;
    view.taskName = summary.exc_task;
    view.excPc = summary.exc_pc;
    view.bt = summary.exc_bt_info.bt;
    view.depth = summary.exc_bt_info.depth;
    view.btCapacity = sizeof(summary.exc_bt_info.bt) / sizeof(summary.exc_bt_info.bt[0]);
    view.corrupted = summary.exc_bt_info.corrupted;
    view.elfSha256 = (const char *)summary.app_elf_sha256;

    // SerialDevice::println takes a String; wrap explicitly rather than leaning on
    // the implicit const char* conversion against its integer overloads.
    serialDevice->println(String(formatCrashSummary(view).c_str()));

    // The decode is only valid if the stored dump came from the firmware now
    // running, and the repo's rule is to check that before trusting any addr2line
    // output. Both values come from the same app-descriptor field the panic handler
    // prints, so comparing them here settles it on-device -- a host-side
    // `sha256sum firmware.elf` is a different digest and would not match.
    const char *running = esp_app_get_elf_sha256_str();
    serialDevice->printf(
        "crash: running_elf=%s match=%s\r\n",
        running ? running : "?",
        (running && strncmp(running, (const char *)summary.app_elf_sha256, strlen(running)) == 0)
            ? "yes"
            : "NO -- decode against this build is invalid"
    );
    return true;
}

void reportBootCrashState() {
    const int reason = (int)esp_reset_reason();
    log_e(
        "[BOOT] reset_reason=%s(%d) coredump=%s",
        resetReasonName(reason),
        reason,
        haveStoredDump() ? "stored" : "none"
    );
}

#else // core dump not compiled in

static uint32_t crashlogCallback(cmd *c) {
    // An absent capability must say so. A verb that silently disappears on another
    // board profile is the ISSUE-4 mistake in a new place.
    const int reason = (int)esp_reset_reason();
    serialDevice->printf("crash: reset_reason=%s(%d)\r\n", resetReasonName(reason), reason);
    serialDevice->println("crash: core dump not compiled in for this build");
    return true;
}

void reportBootCrashState() {
    const int reason = (int)esp_reset_reason();
    log_e("[BOOT] reset_reason=%s(%d) coredump=disabled", resetReasonName(reason), reason);
}

#endif

void createCrashCommands(SimpleCLI *cli) {
    Command crashCmd = cli->addCommand("crashlog", crashlogCallback);
    crashCmd.addFlagArg("clear");
    crashCmd.addFlagArg("selftest");
}
