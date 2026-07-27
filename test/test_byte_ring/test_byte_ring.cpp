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

// contains() backs the line-framing fix in BLESerialService: available() must
// report "a command is ready" rather than "some bytes arrived", so a command
// split across several characteristic writes is not parsed as a fragment.

void test_contains_finds_terminator() {
    ByteRing<8> r;
    const uint8_t in[] = {'i', 'n', 'f', 'o', '\n'};
    r.write(in, 5);
    TEST_ASSERT_TRUE(r.contains('\n'));
}

void test_contains_false_without_terminator() {
    ByteRing<8> r;
    const uint8_t in[] = {'s', 'y', 's', 't', 'e', 'm'};
    r.write(in, 6);
    TEST_ASSERT_FALSE(r.contains('\n'));
}

void test_contains_false_when_empty() {
    ByteRing<8> r;
    TEST_ASSERT_FALSE(r.contains('\n'));
}

// contains() is a predicate, not a read. A version that consumed while scanning
// would pass the two tests above and still destroy the buffer.
void test_contains_does_not_consume() {
    ByteRing<8> r;
    const uint8_t in[] = {'h', 'i', '\n'};
    r.write(in, 3);
    TEST_ASSERT_TRUE(r.contains('\n'));
    TEST_ASSERT_EQUAL(3, r.size());
    TEST_ASSERT_TRUE(r.contains('\n')); // still true on a second call
    TEST_ASSERT_EQUAL('h', r.read());
    TEST_ASSERT_EQUAL('i', r.read());
    TEST_ASSERT_EQUAL('\n', r.read());
}

// The terminator sits past the wrap point, so scanning buf[0..count) instead of
// walking from head with modulo indexing reads the wrong slots and misses it.
void test_contains_finds_terminator_across_wrap() {
    ByteRing<4> r;
    const uint8_t in[] = {'a', 'b', 'c'};
    r.write(in, 3);
    r.read(); // consume 'a'
    r.read(); // consume 'b' — head is now at index 2
    const uint8_t more[] = {'d', '\n'};
    r.write(more, 2); // 'd' at index 3, '\n' wraps to index 0
    TEST_ASSERT_TRUE(r.contains('\n'));
    TEST_ASSERT_FALSE(r.contains('z'));
}

// A stale terminator left in an already-consumed slot must not register.
void test_contains_ignores_consumed_bytes() {
    ByteRing<8> r;
    const uint8_t in[] = {'x', '\n', 'y'};
    r.write(in, 3);
    r.read(); // 'x'
    r.read(); // '\n' — consumed; only 'y' remains unread
    TEST_ASSERT_FALSE(r.contains('\n'));
}

int main(int, char **) {
    UNITY_BEGIN();
    RUN_TEST(test_starts_empty);
    RUN_TEST(test_write_then_read_fifo);
    RUN_TEST(test_wraps_around);
    RUN_TEST(test_drops_overflow_and_reports_accepted);
    RUN_TEST(test_clear_empties);
    RUN_TEST(test_overflow_preserves_unread_bytes);
    RUN_TEST(test_contains_finds_terminator);
    RUN_TEST(test_contains_false_without_terminator);
    RUN_TEST(test_contains_false_when_empty);
    RUN_TEST(test_contains_does_not_consume);
    RUN_TEST(test_contains_finds_terminator_across_wrap);
    RUN_TEST(test_contains_ignores_consumed_bytes);
    return UNITY_END();
}
