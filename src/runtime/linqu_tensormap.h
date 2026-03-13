#ifndef LINQU_RUNTIME_TENSORMAP_H
#define LINQU_RUNTIME_TENSORMAP_H

#include <cstdint>
#include <vector>

namespace linqu {

struct LinquTensorMapEntry {
    uint64_t tensor_handle;
    int32_t producer_task_id;
    int32_t next_in_bucket;  // -1 = end
    bool valid;
};

struct LinquTensorMapLookupResult {
    int32_t producer_task_id;
    bool found;
};

struct LinquTensorMap {
    void init(int32_t num_buckets, int32_t pool_size);
    void reset();

    void insert(uint64_t handle, int32_t producer_task_id);
    LinquTensorMapLookupResult lookup(uint64_t handle) const;
    void invalidate_below(int32_t last_task_alive);

    int32_t valid_count() const;

private:
    int32_t hash(uint64_t handle) const;

    std::vector<int32_t> buckets_;
    std::vector<LinquTensorMapEntry> pool_;
    int32_t num_buckets_ = 0;
    int32_t pool_size_ = 0;
    int32_t next_entry_ = 0;
    int32_t last_task_alive_ = 0;
};

}

#endif
