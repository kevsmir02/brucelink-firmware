#include "core/crash_report.h"
#include <string.h>
#include <unity.h>

// The formatter is the only part of the crash path that can be tested off-device:
// esp_core_dump_get_summary() needs a real panic and real flash.

static const uint32_t kBt[4] = {0x420a1c34u, 0x420a0f88u, 0x4037d1a0u, 0x4037c904u};

static CrashSummaryView baseView() {
    CrashSummaryView v;
    v.taskName = "main";
    v.excPc = 0x420a1c34u;
    v.bt = kBt;
    v.depth = 4;
    v.btCapacity = 16;
    v.corrupted = false;
    v.elfSha256 = "2841bf2b5";
    return v;
}

void test_renders_header_fields() {
    std::string s = formatCrashSummary(baseView());
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "task=main"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "pc=0x420a1c34"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "elf=2841bf2b5"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "depth=4"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "corrupted=no"));
}

void test_renders_backtrace_lowercase_in_order() {
    std::string s = formatCrashSummary(baseView());
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "bt=0x420a1c34 0x420a0f88 0x4037d1a0 0x4037c904"));
}

// A dump with depth 0 and a null bt must not be dereferenced. This is the shape a
// corrupted dump arrives in.
void test_zero_depth_emits_no_backtrace_line() {
    CrashSummaryView v = baseView();
    v.depth = 0;
    v.bt = nullptr;
    std::string s = formatCrashSummary(v);
    TEST_ASSERT_NULL(strstr(s.c_str(), "bt="));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "depth=0"));
}

// depth is read straight out of the dump, so a corrupt value must not walk off the
// end of bt[].
void test_depth_clamps_to_capacity() {
    CrashSummaryView v = baseView();
    v.depth = 99;
    v.btCapacity = 4;
    std::string s = formatCrashSummary(v);
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "depth=4"));
    TEST_ASSERT_NULL(strstr(s.c_str(), "0x00000000"));
}

void test_corrupted_flag_renders_yes() {
    CrashSummaryView v = baseView();
    v.corrupted = true;
    TEST_ASSERT_NOT_NULL(strstr(formatCrashSummary(v).c_str(), "corrupted=yes"));
}

void test_missing_strings_render_placeholder_not_garbage() {
    CrashSummaryView v = baseView();
    v.taskName = "";
    v.elfSha256 = nullptr;
    std::string s = formatCrashSummary(v);
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "task=?"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "elf=?"));
}

void test_reset_reason_names() {
    TEST_ASSERT_EQUAL_STRING("panic", resetReasonName(4));
    TEST_ASSERT_EQUAL_STRING("poweron", resetReasonName(1));
    TEST_ASSERT_EQUAL_STRING("task_wdt", resetReasonName(6));
    TEST_ASSERT_EQUAL_STRING("brownout", resetReasonName(9));
    TEST_ASSERT_EQUAL_STRING("unknown", resetReasonName(255));
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_renders_header_fields);
    RUN_TEST(test_renders_backtrace_lowercase_in_order);
    RUN_TEST(test_zero_depth_emits_no_backtrace_line);
    RUN_TEST(test_depth_clamps_to_capacity);
    RUN_TEST(test_corrupted_flag_renders_yes);
    RUN_TEST(test_missing_strings_render_placeholder_not_garbage);
    RUN_TEST(test_reset_reason_names);
    return UNITY_END();
}
