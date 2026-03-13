#ifndef LINQU_PROFILING_RING_METRICS_H
#define LINQU_PROFILING_RING_METRICS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>

namespace linqu {

struct RingMetrics {
    uint8_t  level = 0;
    uint16_t depth = 0;

    size_t task_ring_capacity     = 0;
    size_t task_ring_peak_used    = 0;
    size_t task_alloc_count       = 0;
    size_t task_retire_count      = 0;
    size_t task_block_count       = 0;

    size_t buffer_ring_capacity   = 0;
    size_t buffer_ring_peak_used  = 0;
    size_t buffer_alloc_count     = 0;
    size_t buffer_alloc_bytes     = 0;
    size_t buffer_retire_bytes    = 0;

    size_t scope_begin_count      = 0;
    size_t scope_end_count        = 0;
    size_t free_tensor_count      = 0;
};

struct RingSnapshot {
    uint8_t  level = 0;
    uint16_t depth = 0;
    std::string label;
    uint64_t timestamp_us = 0;

    size_t task_capacity     = 0;
    size_t task_used         = 0;
    size_t task_head         = 0;
    size_t task_tail         = 0;

    size_t buffer_capacity   = 0;
    size_t buffer_used       = 0;
    size_t buffer_head       = 0;
    size_t buffer_tail       = 0;

    size_t alloc_count       = 0;
    size_t retire_count      = 0;
    size_t free_count        = 0;
};

struct ProfileReport {
    std::string node_id;
    uint8_t level = 0;
    std::vector<RingMetrics> metrics;
    std::vector<RingSnapshot> snapshots;

    std::string to_json() const;
};

inline uint64_t now_us() {
    auto tp = std::chrono::steady_clock::now();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            tp.time_since_epoch()).count());
}

}

#endif
