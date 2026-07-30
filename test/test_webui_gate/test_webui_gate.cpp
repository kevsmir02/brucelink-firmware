#include "core/wifi/webui_gate.h"
#include <string.h>
#include <unity.h>

// The gates themselves read the heap and touch WiFi, so only the predicates and
// the formatter can run here. Figures below are ISSUE-12's measured runs.

static WebUiStartReport baseReport() {
    WebUiStartReport r;
    r.result = WebUiStartResult::Started;
    r.dmaBlock = 6900;
    r.required = 15360;
    r.tcpState = 1;
    r.apMode = true;
    return r;
}

void test_listening_only_on_lwip_listen() {
    TEST_ASSERT_TRUE(webUiListening(1));  // LISTEN
    TEST_ASSERT_FALSE(webUiListening(0)); // CLOSED: a failed begin() leaves _pcb null
    TEST_ASSERT_FALSE(webUiListening(4)); // ESTABLISHED is not a listening socket
}

void test_dma_threshold_is_inclusive() {
    TEST_ASSERT_TRUE(webUiDmaSufficient(15360, 15360));
    TEST_ASSERT_FALSE(webUiDmaSufficient(15359, 15360));
    TEST_ASSERT_FALSE(webUiDmaSufficient(1844, 15360)); // 2026-07-29 failing run
    // 2026-07-30 run 2: clears the gate and still failed downstream. Pinned so a
    // later change cannot quietly "fix" that by inventing a higher threshold.
    TEST_ASSERT_TRUE(webUiDmaSufficient(18420, 15360));
}

void test_every_result_has_a_distinct_slug() {
    const char *a = webUiResultSlug(WebUiStartResult::Started);
    const char *b = webUiResultSlug(WebUiStartResult::WifiBringUpFailed);
    const char *c = webUiResultSlug(WebUiStartResult::RefusedLowDmaPreAlloc);
    const char *d = webUiResultSlug(WebUiStartResult::FailedNotListening);
    TEST_ASSERT_EQUAL_STRING("started", a);
    TEST_ASSERT_EQUAL_STRING("wifi_bringup_failed", b);
    TEST_ASSERT_EQUAL_STRING("low_dma_pre_alloc", c);
    TEST_ASSERT_EQUAL_STRING("not_listening", d);
}

void test_report_renders_every_field_on_one_line() {
    WebUiStartReport r = baseReport();
    r.result = WebUiStartResult::RefusedLowDmaPreAlloc;
    r.dmaBlock = 1844;
    r.tcpState = 0;
    std::string s = formatWebUiStartReport(r);
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "low_dma_pre_alloc"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "dma=1844"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "required=15360"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "mode=ap"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "tcp_state=0"));
    // Single line: a BLE reply can truncate (ISSUE-16) and the first line must carry
    // the whole verdict.
    TEST_ASSERT_NULL(strchr(s.c_str(), '\n'));
    TEST_ASSERT_NULL(strchr(s.c_str(), '\r'));
}

void test_report_distinguishes_sta_from_ap() {
    WebUiStartReport r = baseReport();
    r.apMode = false;
    TEST_ASSERT_NOT_NULL(strstr(formatWebUiStartReport(r).c_str(), "mode=sta"));
    r.apMode = true;
    TEST_ASSERT_NOT_NULL(strstr(formatWebUiStartReport(r).c_str(), "mode=ap"));
}

void test_success_report_carries_the_post_begin_dma_figure() {
    // Reported on success too, and deliberately not gated on: ISSUE-12 records only
    // as SUSPECTED that this figure predicts the outcome (6900/6644 served, 6132 did
    // not) on one set of three runs.
    WebUiStartReport r = baseReport();
    std::string s = formatWebUiStartReport(r);
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "started"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "dma=6900"));
    TEST_ASSERT_NOT_NULL(strstr(s.c_str(), "tcp_state=1"));
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_listening_only_on_lwip_listen);
    RUN_TEST(test_dma_threshold_is_inclusive);
    RUN_TEST(test_every_result_has_a_distinct_slug);
    RUN_TEST(test_report_renders_every_field_on_one_line);
    RUN_TEST(test_report_distinguishes_sta_from_ap);
    RUN_TEST(test_success_report_carries_the_post_begin_dma_figure);
    return UNITY_END();
}
