#include "modules/ble_api/services/ByteRing.h"
#include <unity.h>

void test_starts_empty() {
    ByteRing<8> r;
    TEST_ASSERT_TRUE(r.empty());
    TEST_ASSERT_EQUAL(0, r.size());
    TEST_ASSERT_EQUAL(-1, r.read());
}

void test_write_then_read_fifo() {
    ByteRing<8> r;
    const uint8_t in[] = {'a', 'b', 'c'};
    TEST_ASSERT_EQUAL(3, r.write(in, 3));
    TEST_ASSERT_EQUAL(3, r.size());
    TEST_ASSERT_EQUAL('a', r.read());
    TEST_ASSERT_EQUAL('b', r.read());
    TEST_ASSERT_EQUAL('c', r.read());
    TEST_ASSERT_EQUAL(-1, r.read());
}

void test_wraps_around() {
    ByteRing<4> r;
    const uint8_t in[] = {'1', '2', '3'};
    r.write(in, 3);
    r.read();
    r.read();
    const uint8_t more[] = {'4', '5'};
    TEST_ASSERT_EQUAL(2, r.write(more, 2));
    TEST_ASSERT_EQUAL('3', r.read());
    TEST_ASSERT_EQUAL('4', r.read());
    TEST_ASSERT_EQUAL('5', r.read());
}

void test_drops_overflow_and_reports_accepted() {
    ByteRing<4> r;
    const uint8_t in[] = {'1', '2', '3', '4', '5', '6'};
    TEST_ASSERT_EQUAL(4, r.write(in, 6));
    TEST_ASSERT_EQUAL(4, r.size());
    TEST_ASSERT_EQUAL('1', r.read());
}

void test_clear_empties() {
    ByteRing<4> r;
    const uint8_t in[] = {'x', 'y'};
    r.write(in, 2);
    r.clear();
    TEST_ASSERT_TRUE(r.empty());
    TEST_ASSERT_EQUAL(-1, r.read());
}

// Regression test for a gap the code review flagged: test_drops_overflow_and_reports_accepted
// starts from an EMPTY buffer, so a buggy write() that overwrote unread bytes on overflow
// would produce identical observable results. This test leaves one unread byte in the buffer
// before overflowing it, so a bad implementation would clobber that byte and be caught here.
void test_overflow_preserves_unread_bytes() {
    ByteRing<4> r;
    const uint8_t in[] = {'a', 'b'};
    r.write(in, 2);
    r.read();  // consume 'a'; 'b' remains unread, 3 slots free
    const uint8_t more[] = {'1', '2', '3', '4', '5'};
    TEST_ASSERT_EQUAL(3, r.write(more, 5));  // only 3 of 5 fit
    TEST_ASSERT_EQUAL(4, r.size());
    TEST_ASSERT_EQUAL('b', r.read());  // pre-existing unread byte, unclobbered
    TEST_ASSERT_EQUAL('1', r.read());
    TEST_ASSERT_EQUAL('2', r.read());
    TEST_ASSERT_EQUAL('3', r.read());
    TEST_ASSERT_EQUAL(-1, r.read());
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_empty);
    RUN_TEST(test_write_then_read_fifo);
    RUN_TEST(test_wraps_around);
    RUN_TEST(test_drops_overflow_and_reports_accepted);
    RUN_TEST(test_clear_empties);
    RUN_TEST(test_overflow_preserves_unread_bytes);
    return UNITY_END();
}
