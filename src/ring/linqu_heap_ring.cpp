#include "ring/linqu_heap_ring.h"
#include <cassert>

namespace linqu {

void LinquHeapRing::init(size_t capacity_bytes) {
    store_.resize(capacity_bytes, 0);
    capacity_ = capacity_bytes;
    top_ = 0;
    tail_ = 0;
}

void LinquHeapRing::reset() {
    top_ = 0;
    tail_ = 0;
}

size_t LinquHeapRing::used() const {
    if (top_ >= tail_) return top_ - tail_;
    return capacity_ - tail_ + top_;
}

size_t LinquHeapRing::available() const {
    return capacity_ - used();
}

bool LinquHeapRing::try_alloc(size_t size, void** out) {
    if (size == 0 || size > capacity_) return false;
    if (available() < size) return false;

    size_t pos = top_ % capacity_;
    if (pos + size <= capacity_) {
        *out = store_.data() + pos;
        top_ += size;
        return true;
    }
    size_t waste = capacity_ - pos;
    if (available() < size + waste) return false;
    top_ += waste;
    pos = top_ % capacity_;
    *out = store_.data() + pos;
    top_ += size;
    return true;
}

void* LinquHeapRing::alloc(size_t size) {
    void* p = nullptr;
    bool ok = try_alloc(size, &p);
    assert(ok && "LinquHeapRing::alloc failed — ring full");
    return p;
}

void LinquHeapRing::retire_to(size_t new_tail) {
    tail_ = new_tail;
}

}
