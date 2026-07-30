#if !defined(LITE_VERSION) && !defined(DISABLE_INTERPRETER)
#include "serial_js.h"

#include "core/display.h"

#include "core/wifi/ws_events.h"
#include "helpers_js.h"
#include <globals.h>

// Emitted as an event frame rather than to Serial. Serial is the USB CDC object,
// which reaches nothing at all on this board, so `serial.print()` from a script
// produced no output on any channel (ISSUE-15).
//
// The event stream, not serialDevice: scripts run on the interpreter task, so their
// output appears long after `js run_from_buffer` has already written its reply and
// its 0x04 EOT. Writing to the CLI characteristic there would inject bytes into
// whatever command came next and desynchronise the framing — the same hazard that
// makes `display start` unusable. Events are a separate characteristic for exactly
// this reason.
static void internal_print_mq(JSContext *ctx, int argc, JSValue *argv, uint8_t printTft, uint8_t newLine) {
    String out;
    for (int argIndex = 0; argIndex < argc && argIndex < 20; ++argIndex) {
        if (argIndex > 0) out += ' ';

        JSValue v = argv[argIndex];
        if (JS_IsUndefined(v)) {
            out += "undefined";
        } else if (JS_IsNull(v)) {
            out += "null";
        } else if (JS_IsNumber(ctx, v)) {
            double num;
            JS_ToNumber(ctx, &num, v);
            char numBuf[32];
            snprintf(numBuf, sizeof(numBuf), "%g", num);
            out += numBuf;
        } else if (JS_IsBool(v)) {
            out += (JS_ToBool(ctx, v) ? "true" : "false");
        } else {
            JSCStringBuf sb;
            const char *s = JS_ToCString(ctx, v, &sb);
            if (s) out += s;
        }
    }

    if (printTft) {
        if (newLine) tft.println(out);
        else tft.print(out);
    }
    // Partial writes are accumulated so a print()/println() pair still arrives as one
    // readable line, rather than one frame per fragment.
    static String pending;
    pending += out;
    if (newLine) {
        pushWsLog("[js] " + pending, "info");
        pending = "";
    }
}

JSValue native_serialPrint(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    internal_print_mq(ctx, argc, argv, false, false);
    return JS_UNDEFINED;
}

JSValue native_serialPrintln(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    internal_print_mq(ctx, argc, argv, false, true);
    return JS_UNDEFINED;
}

JSValue native_serialReadln(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    String line;
    int maxloops = 1000 * 10;
    if (argc > 0 && JS_IsNumber(ctx, argv[0])) {
        int t;
        JS_ToInt32(ctx, &t, argv[0]);
        maxloops = t;
    }
    Serial.flush();
    while (maxloops) {
        if (!Serial.available()) {
            maxloops -= 1;
            delay(1);
            continue;
        }
        line = Serial.readStringUntil('\n');
        break;
    }
    return JS_NewString(ctx, line.c_str());
}

JSValue native_serialCmd(JSContext *ctx, JSValue *this_val, int argc, JSValue *argv) {
    const char *cmd = NULL;
    JSCStringBuf sb;
    if (argc > 0 && JS_IsString(ctx, argv[0])) cmd = JS_ToCString(ctx, argv[0], &sb);
    bool r = false;
    if (cmd) r = serialCli.parse(String(cmd));
    return JS_NewBool(r);
}

#endif
