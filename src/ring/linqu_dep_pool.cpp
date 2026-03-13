#include "ring/linqu_dep_pool.h"
#include <cassert>

namespace linqu {

void LinquDepListPool::init(int32_t cap) {
    assert(cap > 0);
    capacity_ = cap;
    pool_.resize(static_cast<size_t>(cap + 1));
    top_ = 1;
}

void LinquDepListPool::reset() {
    top_ = 1;
}

int32_t LinquDepListPool::alloc_one() {
    if (top_ > capacity_) return 0;
    int32_t idx = top_++;
    pool_[idx] = LinquDepListEntry{-1, -1};
    return idx;
}

int32_t LinquDepListPool::prepend(int32_t current_head, int32_t task_id) {
    int32_t idx = alloc_one();
    assert(idx > 0);
    pool_[idx].task_id = task_id;
    pool_[idx].next = current_head;
    return idx;
}

const LinquDepListEntry* LinquDepListPool::get(int32_t offset) const {
    if (offset <= 0) return nullptr;
    return &pool_[offset];
}

LinquDepListEntry* LinquDepListPool::get(int32_t offset) {
    if (offset <= 0) return nullptr;
    return &pool_[offset];
}

int32_t LinquDepListPool::count(int32_t head) const {
    int32_t n = 0;
    int32_t cur = head;
    while (cur > 0) {
        n++;
        cur = pool_[cur].next;
    }
    return n;
}

}
