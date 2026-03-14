#ifndef LINQU_RING_TASK_RING_H
#define LINQU_RING_TASK_RING_H

#include "core/task_key.h"
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
#include <string>

namespace linqu {

struct LinquTaskDescriptor {
    TaskKey key;

    // --- Fanin: producer dependencies (set once at submit, read-only after) ---
    int32_t dep_list_head = -1;    // DepListPool offset; linked list of producers
    uint32_t fanin_count = 0;      // number of producer dependencies

    // --- Fanout: consumer tasks + scope ref (grows as consumers are added) ---
    int32_t fanout_list_head = -1; // DepListPool offset; linked list of consumers
    uint32_t fanout_count = 1;     // 1 (scope ref) + N (consumer tasks)

    // Atomic refcounts — aligned with simpler's multi-threaded protocol.
    // fanin_refcount: incremented when each producer completes.
    //   Task becomes READY when atomic_fanin_refcount == fanin_count.
    // fanout_refcount: incremented when each consumer completes + scope_end.
    //   Task becomes CONSUMED when atomic_fanout_refcount == fanout_count.
    std::atomic<uint32_t> atomic_fanin_refcount{0};
    std::atomic<uint32_t> atomic_fanout_refcount{0};

    // Per-task spinlock protecting fanout_list_head/fanout_count.
    // Aligns with simpler's pto2_fanout_lock/unlock pattern: the orchestrator
    // may add consumers while a worker thread's completion propagation is
    // traversing the fanout chain.
    volatile int32_t fanout_lock{0};

    bool task_freed = false;
    std::string kernel_so;
    size_t output_offset = 0;
    size_t output_size = 0;
    size_t heap_end = 0;

    enum class Status : uint8_t { PENDING, READY, RUNNING, COMPLETED, CONSUMED };
    Status status = Status::PENDING;

    // L3-L7 worker lambda (CPU execution).  Necessarily different from
    // simpler's kernel_id (hardware core dispatch) and the existing
    // kernel_so (dlopen .so dispatch).  All three may coexist.
    std::function<void()> fn;

    // Legacy non-atomic fields kept for backward compatibility with
    // OrchestratorState and existing tests that run single-threaded.
    uint32_t fanout_refcount = 0;
    uint32_t fanin_refcount = 0;
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
    std::vector<std::unique_ptr<LinquTaskDescriptor>> descriptors_;
    int32_t window_size_ = 0;
    int32_t current_index_ = 0;
    int32_t last_task_alive_ = 0;
};

}

#endif
