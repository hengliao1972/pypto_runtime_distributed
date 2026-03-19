#include "ring/linqu_task_ring.h"
#include <cassert>

namespace linqu {

static void reset_descriptor(LinquTaskDescriptor& d) {
    d.key = TaskKey{};
    d.dep_list_head = -1;
    d.fanin_count = 0;
    d.fanout_list_head = -1;
    d.fanout_count = 1;
    d.atomic_fanin_refcount.store(0, std::memory_order_relaxed);
    d.atomic_fanout_refcount.store(0, std::memory_order_relaxed);
    d.fanout_lock = 0;
    d.task_freed = false;
    d.kernel_so.clear();
    d.output_offset = 0;
    d.output_size = 0;
    d.heap_end = 0;
    d.status = LinquTaskDescriptor::Status::PENDING;
    d.fn = nullptr;
    d.fanout_refcount = 0;
    d.fanin_refcount = 0;
    d.is_group = false;
    d.group_size = 0;
    d.sub_complete_count.store(0, std::memory_order_relaxed);
}

void LinquTaskRing::init(int32_t ws) {
    assert(ws > 0 && (ws & (ws - 1)) == 0);
    window_size_ = ws;
    descriptors_.clear();
    descriptors_.reserve(static_cast<size_t>(ws));
    for (int32_t i = 0; i < ws; i++)
        descriptors_.push_back(std::make_unique<LinquTaskDescriptor>());
    current_index_ = 0;
    last_task_alive_ = 0;
}

void LinquTaskRing::reset() {
    current_index_ = 0;
    last_task_alive_ = 0;
    for (auto& p : descriptors_) {
        reset_descriptor(*p);
    }
}

int32_t LinquTaskRing::active_count() const {
    return current_index_ - last_task_alive_;
}

bool LinquTaskRing::has_space() const {
    return active_count() < window_size_;
}

int32_t LinquTaskRing::try_alloc() {
    if (!has_space()) return -1;
    int32_t id = current_index_++;
    int32_t slot = id & (window_size_ - 1);
    reset_descriptor(*descriptors_[slot]);
    return id;
}

int32_t LinquTaskRing::alloc() {
    int32_t id = try_alloc();
    assert(id >= 0 && "LinquTaskRing::alloc failed — window full");
    return id;
}

LinquTaskDescriptor* LinquTaskRing::get(int32_t task_id) {
    return descriptors_[task_id & (window_size_ - 1)].get();
}

const LinquTaskDescriptor* LinquTaskRing::get(int32_t task_id) const {
    return descriptors_[task_id & (window_size_ - 1)].get();
}

}
