#ifndef LINQU_RING_TASK_RING_H
#define LINQU_RING_TASK_RING_H

#include "core/task_key.h"
#include <cstdint>
#include <vector>
#include <string>

namespace linqu {

struct LinquTaskDescriptor {
    TaskKey key;

    uint32_t fanout_count = 1;     // 1 (scope) + N (consumer tasks)
    uint32_t fanout_refcount = 0;  // incremented by scope_end + consumer completion
    uint32_t fanin_count = 0;      // number of producer dependencies
    uint32_t fanin_refcount = 0;   // incremented when producers complete

    bool task_freed = false;
    int32_t dep_list_head = -1;    // linked list of producers (fanin list)
    int32_t fanout_list_head = -1; // linked list of consumers (fanout list)
    std::string kernel_so;
    size_t output_offset = 0;
    size_t output_size = 0;
    size_t heap_end = 0;

    enum class Status : uint8_t { PENDING, READY, RUNNING, COMPLETED, CONSUMED };
    Status status = Status::PENDING;
};

struct LinquTaskRing {
    void init(int32_t window_size);
    void reset();

    int32_t alloc();
    int32_t try_alloc();

    LinquTaskDescriptor* get(int32_t task_id);
    const LinquTaskDescriptor* get(int32_t task_id) const;

    int32_t active_count() const;
    bool has_space() const;

    int32_t window_size() const { return window_size_; }
    int32_t current_index() const { return current_index_; }
    int32_t last_task_alive() const { return last_task_alive_; }
    void set_last_task_alive(int32_t v) { last_task_alive_ = v; }

private:
    std::vector<LinquTaskDescriptor> descriptors_;
    int32_t window_size_ = 0;
    int32_t current_index_ = 0;
    int32_t last_task_alive_ = 0;
};

}

#endif
