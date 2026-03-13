#ifndef LINQU_RUNTIME_TENSORMAP_H
#define LINQU_RUNTIME_TENSORMAP_H

#include <cstdint>
#include <vector>

namespace linqu {

struct LinquTensorMapEntry {
    uint64_t tensor_handle;
    int32_t producer_task_id;
    int32_t next_in_bucket;   // bucket chain: -1 = end
    int32_t next_in_task;     // per-task chain: -1 = end
    bool valid;
    bool is_output_alloc;     // true if this entry came from an OUTPUT (not INOUT reuse)
};

struct LinquTensorMapLookupResult {
    int32_t producer_task_id;
    bool found;
};

struct LinquTensorMap {
    void init(int32_t num_buckets, int32_t pool_size);
    void reset();

    void insert(uint64_t handle, int32_t producer_task_id,
                bool is_output = true);
    LinquTensorMapLookupResult lookup(uint64_t handle) const;
    void invalidate_below(int32_t last_task_alive);
    void cleanup_task(int32_t task_id);

    int32_t task_chain_head(int32_t task_id) const;
    int32_t valid_count() const;

private:
    int32_t hash(uint64_t handle) const;
    void remove_stale_for_handle(uint64_t handle, int32_t new_task_id);

    std::vector<int32_t> buckets_;
    std::vector<LinquTensorMapEntry> pool_;
    std::vector<int32_t> task_heads_; // per-task chain heads, indexed by task_id % pool_size
    int32_t num_buckets_ = 0;
    int32_t pool_size_ = 0;
    int32_t next_entry_ = 0;
    int32_t last_task_alive_ = 0;
};

}

#endif
