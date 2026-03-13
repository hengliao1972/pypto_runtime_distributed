#ifndef LINQU_RING_DEP_POOL_H
#define LINQU_RING_DEP_POOL_H

#include <cstdint>
#include <vector>

namespace linqu {

struct LinquDepListEntry {
    int32_t task_id;
    int32_t next;  // offset to next entry, -1 = end
};

struct LinquDepListPool {
    void init(int32_t capacity);
    void reset();

    int32_t alloc_one();
    int32_t prepend(int32_t current_head, int32_t task_id);

    const LinquDepListEntry* get(int32_t offset) const;
    LinquDepListEntry* get(int32_t offset);

    int32_t count(int32_t head) const;
    int32_t used() const { return top_ - 1; }
    int32_t available() const { return capacity_ - used(); }

private:
    std::vector<LinquDepListEntry> pool_;
    int32_t capacity_ = 0;
    int32_t top_ = 1;  // 0 is reserved as NULL
};

}

#endif
