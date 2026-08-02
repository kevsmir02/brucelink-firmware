#ifndef __BYTE_RING_H__
#define __BYTE_RING_H__

#include <stddef.h>
#include <stdint.h>

// Fixed-capacity byte FIFO. Header-only and free of Arduino dependencies so it
// can be unit-tested under [env:native]. Overflow drops the tail of the incoming
// write rather than overwriting unread bytes: a truncated command fails to parse
// loudly, whereas overwriting would silently corrupt an in-flight one.
template <size_t N> class ByteRing {
    uint8_t buf[N]{};  // zero-init so no uninit bytes before the first write
                       //  (cppcheck uninitMemberVarNoCtor; head/tail/count are
                       //  the validity tracker, but on a no-MMU ESP32-S3 an
                       //  uninit read is UB, not a noisy warning)
    size_t head = 0;  // next read index
    size_t tail = 0;  // next write index
    size_t count = 0;

public:
    size_t size() const { return count; }
    size_t capacity() const { return N; }
    bool empty() const { return count == 0; }

    void clear() {
        head = tail = count = 0;
    }

    // Returns the number of bytes actually accepted.
    size_t write(const uint8_t *data, size_t len) {
        size_t accepted = 0;
        for (size_t i = 0; i < len && count < N; ++i) {
            buf[tail] = data[i];
            tail = (tail + 1) % N;
            ++count;
            ++accepted;
        }
        return accepted;
    }

    // True if an unread byte equals `needle`. Non-mutating: callers use it to ask
    // "is a complete line buffered?" before deciding to consume one. Walks from
    // head with modulo indexing, so it stays correct across the wrap point and
    // never sees already-consumed slots.
    bool contains(uint8_t needle) const {
        for (size_t i = 0, idx = head; i < count; ++i, idx = (idx + 1) % N) {
            if (buf[idx] == needle) return true;
        }
        return false;
    }

    // Returns -1 when empty.
    int read() {
        if (count == 0) return -1;
        uint8_t v = buf[head];
        head = (head + 1) % N;
        --count;
        return (int)v;
    }
};

#endif
