#include "ring/linqu_task_ring.h"
#include <cassert>

namespace linqu {

void LinquTaskRing::init(int32_t ws) {
    assert(ws > 0 && (ws & (ws - 1)) == 0);
    window_size_ = ws;
    descriptors_.resize(static_cast<size_t>(ws));
    current_index_ = 0;
    last_task_alive_ = 0;
}

void LinquTaskRing::reset() {
    current_index_ = 0;
    last_task_alive_ = 0;
    for (auto& d : descriptors_) {
        d = LinquTaskDescriptor{};
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
    descriptors_[slot] = LinquTaskDescriptor{};
    return id;
}

int32_t LinquTaskRing::alloc() {
    int32_t id = try_alloc();
    assert(id >= 0 && "LinquTaskRing::alloc failed — window full");
    return id;
}

LinquTaskDescriptor* LinquTaskRing::get(int32_t task_id) {
    return &descriptors_[task_id & (window_size_ - 1)];
}

const LinquTaskDescriptor* LinquTaskRing::get(int32_t task_id) const {
    return &descriptors_[task_id & (window_size_ - 1)];
}

}
