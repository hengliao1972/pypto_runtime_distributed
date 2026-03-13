#ifndef LINQU_RING_HEAP_RING_H
#define LINQU_RING_HEAP_RING_H

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

namespace linqu {

struct LinquHeapRing {
    void init(size_t capacity_bytes);
    void reset();

    void* alloc(size_t size);
    bool try_alloc(size_t size, void** out);
    void retire_to(size_t new_tail);

    size_t capacity() const { return capacity_; }
    size_t used() const;
    size_t available() const;
    size_t top() const { return top_; }
    size_t tail() const { return tail_; }

private:
    std::vector<uint8_t> store_;
    size_t capacity_ = 0;
    size_t top_ = 0;
    size_t tail_ = 0;
};

}

#endif
