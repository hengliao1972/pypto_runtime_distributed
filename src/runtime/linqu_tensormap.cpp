#include "runtime/linqu_tensormap.h"
#include <cassert>

namespace linqu {

void LinquTensorMap::init(int32_t num_buckets, int32_t pool_size) {
    assert(num_buckets > 0 && (num_buckets & (num_buckets - 1)) == 0);
    num_buckets_ = num_buckets;
    pool_size_ = pool_size;
    buckets_.assign(static_cast<size_t>(num_buckets), -1);
    pool_.resize(static_cast<size_t>(pool_size));
    task_heads_.assign(static_cast<size_t>(pool_size), -1);
    next_entry_ = 0;
    last_task_alive_ = 0;
}

void LinquTensorMap::reset() {
    std::fill(buckets_.begin(), buckets_.end(), -1);
    std::fill(task_heads_.begin(), task_heads_.end(), -1);
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

void LinquTensorMap::remove_stale_for_handle(uint64_t handle, int32_t new_task_id) {
    int32_t bucket = hash(handle);
    int32_t cur = buckets_[bucket];
    while (cur >= 0) {
        auto& e = pool_[cur];
        if (e.valid && e.tensor_handle == handle &&
            e.producer_task_id >= last_task_alive_ &&
            e.producer_task_id < new_task_id &&
            e.is_output_alloc) {
            e.valid = false;
        }
        cur = e.next_in_bucket;
    }
}

void LinquTensorMap::insert(uint64_t handle, int32_t producer_task_id,
                             bool is_output) {
    remove_stale_for_handle(handle, producer_task_id);

    int32_t idx = next_entry_ % pool_size_;

    if (pool_[idx].valid) {
        int32_t old_bucket = hash(pool_[idx].tensor_handle);
        int32_t* prev_ptr = &buckets_[old_bucket];
        while (*prev_ptr >= 0) {
            if (*prev_ptr == idx) {
                *prev_ptr = pool_[idx].next_in_bucket;
                break;
            }
            prev_ptr = &pool_[*prev_ptr].next_in_bucket;
        }
        pool_[idx].valid = false;
    }

    next_entry_++;

    int32_t bucket = hash(handle);
    pool_[idx].tensor_handle = handle;
    pool_[idx].producer_task_id = producer_task_id;
    pool_[idx].next_in_bucket = buckets_[bucket];
    pool_[idx].valid = true;
    pool_[idx].is_output_alloc = is_output;

    int32_t task_slot = producer_task_id % pool_size_;
    pool_[idx].next_in_task = task_heads_[task_slot];
    task_heads_[task_slot] = idx;

    buckets_[bucket] = idx;
}

LinquTensorMapLookupResult LinquTensorMap::lookup(uint64_t handle) const {
    int32_t bucket = hash(handle);
    int32_t cur = buckets_[bucket];
    int32_t best_task_id = -1;

    while (cur >= 0) {
        const auto& e = pool_[cur];
        if (e.valid && e.tensor_handle == handle
            && e.producer_task_id >= last_task_alive_
            && e.producer_task_id > best_task_id) {
            best_task_id = e.producer_task_id;
        }
        cur = e.next_in_bucket;
    }

    if (best_task_id >= 0) {
        return {best_task_id, true};
    }
    return {-1, false};
}

void LinquTensorMap::invalidate_below(int32_t last_alive) {
    last_task_alive_ = last_alive;
}

void LinquTensorMap::cleanup_task(int32_t task_id) {
    int32_t slot = task_id % pool_size_;
    int32_t cur = task_heads_[slot];
    int32_t prev = -1;
    while (cur >= 0) {
        auto& e = pool_[cur];
        if (e.producer_task_id == task_id) {
            e.valid = false;
        }
        prev = cur;
        cur = e.next_in_task;
    }
    (void)prev;
}

int32_t LinquTensorMap::task_chain_head(int32_t task_id) const {
    return task_heads_[task_id % pool_size_];
}

int32_t LinquTensorMap::valid_count() const {
    int32_t n = 0;
    for (const auto& e : pool_) {
        if (e.valid && e.producer_task_id >= last_task_alive_) n++;
    }
    return n;
}

}
