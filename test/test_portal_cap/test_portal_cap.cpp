#include "modules/wifi/portal_cap.h"
#include <unity.h>

void test_zero_cap_never_expires() {
    TEST_ASSERT_FALSE(portalCapExpired(0, 0xFFFFFFFFu, 0));
}

void test_not_expired_before_cap() {
    TEST_ASSERT_FALSE(portalCapExpired(1000, 1999, 1000));
}

void test_expired_exactly_at_cap() {
    TEST_ASSERT_TRUE(portalCapExpired(1000, 2000, 1000));
}

void test_expired_past_cap() {
    TEST_ASSERT_TRUE(portalCapExpired(1000, 5000, 1000));
}

// millis() wraps at ~49.7 days. Unsigned subtraction stays correct across the
// wrap; a naive nowMs < startedMs guard would not.
void test_expired_across_millis_rollover() {
    TEST_ASSERT_TRUE(portalCapExpired(0xFFFFFF00u, 0x00000100u, 0x100));
}

void test_not_expired_approaching_rollover() {
    TEST_ASSERT_FALSE(portalCapExpired(0xFFFFFF00u, 0xFFFFFF80u, 0x100));
}

void test_remaining_counts_down() {
    TEST_ASSERT_EQUAL_UINT32(400, portalCapRemainingMs(1000, 1600, 1000));
}

void test_remaining_clamps_at_zero() {
    TEST_ASSERT_EQUAL_UINT32(0, portalCapRemainingMs(1000, 9999, 1000));
}

void test_remaining_is_zero_when_uncapped() {
    TEST_ASSERT_EQUAL_UINT32(0, portalCapRemainingMs(1000, 1600, 0));
}

void setUp(void) {}
void tearDown(void) {}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_zero_cap_never_expires);
    RUN_TEST(test_not_expired_before_cap);
    RUN_TEST(test_expired_exactly_at_cap);
    RUN_TEST(test_expired_past_cap);
    RUN_TEST(test_expired_across_millis_rollover);
    RUN_TEST(test_not_expired_approaching_rollover);
    RUN_TEST(test_remaining_counts_down);
    RUN_TEST(test_remaining_clamps_at_zero);
    RUN_TEST(test_remaining_is_zero_when_uncapped);
    return UNITY_END();
}
