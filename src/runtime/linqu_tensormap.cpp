#include "runtime/linqu_tensormap.h"
#include <cassert>

namespace linqu {

void LinquTensorMap::init(int32_t num_buckets, int32_t pool_size) {
    assert(num_buckets > 0 && (num_buckets & (num_buckets - 1)) == 0);
    num_buckets_ = num_buckets;
    pool_size_ = pool_size;
    buckets_.assign(static_cast<size_t>(num_buckets), -1);
    pool_.resize(static_cast<size_t>(pool_size));
    next_entry_ = 0;
    last_task_alive_ = 0;
}

void LinquTensorMap::reset() {
    std::fill(buckets_.begin(), buckets_.end(), -1);
    next_entry_ = 0;
    last_task_alive_ = 0;
}

int32_t LinquTensorMap::hash(uint64_t handle) const {
    uint64_t h = handle;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return static_cast<int32_t>(h & static_cast<uint64_t>(num_buckets_ - 1));
}

void LinquTensorMap::insert(uint64_t handle, int32_t producer_task_id) {
    int32_t idx = next_entry_ % pool_size_;
    next_entry_++;

    int32_t bucket = hash(handle);
    pool_[idx].tensor_handle = handle;
    pool_[idx].producer_task_id = producer_task_id;
    pool_[idx].next_in_bucket = buckets_[bucket];
    pool_[idx].valid = true;
    buckets_[bucket] = idx;
}

LinquTensorMapLookupResult LinquTensorMap::lookup(uint64_t handle) const {
    int32_t bucket = hash(handle);
    int32_t cur = buckets_[bucket];
    while (cur >= 0) {
        const auto& e = pool_[cur];
        if (e.valid && e.tensor_handle == handle
            && e.producer_task_id >= last_task_alive_) {
            return {e.producer_task_id, true};
        }
        cur = e.next_in_bucket;
    }
    return {-1, false};
}

void LinquTensorMap::invalidate_below(int32_t last_alive) {
    last_task_alive_ = last_alive;
}

int32_t LinquTensorMap::valid_count() const {
    int32_t n = 0;
    for (const auto& e : pool_) {
        if (e.valid && e.producer_task_id >= last_task_alive_) n++;
    }
    return n;
}

}
