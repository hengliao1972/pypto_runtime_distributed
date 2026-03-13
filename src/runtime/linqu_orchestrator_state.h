#ifndef LINQU_RUNTIME_ORCHESTRATOR_STATE_H
#define LINQU_RUNTIME_ORCHESTRATOR_STATE_H

#include "core/level.h"
#include "core/coordinate.h"
#include "ring/linqu_heap_ring.h"
#include "ring/linqu_task_ring.h"
#include "ring/linqu_dep_pool.h"
#include "runtime/linqu_scope.h"
#include "runtime/linqu_tensormap.h"
#include "runtime/linqu_orchestration_api.h"
#include "runtime/linqu_dispatcher.h"
#include "profiling/ring_metrics.h"
#include "discovery/peer_registry.h"
#include "discovery/filesystem_discovery.h"
#include <cstdint>
#include <string>
#include <vector>
#include <mutex>
#include <functional>

namespace linqu {

static constexpr int MAX_SCOPE_DEPTH = 8;

struct LinquOrchConfig_Internal {
    int32_t task_ring_window = 64;
    size_t heap_ring_capacity = 1024 * 1024;
    int32_t dep_pool_capacity = 512;
    int32_t tensormap_buckets = 64;
    int32_t tensormap_pool = 256;
};

using TaskDispatchFn = std::function<void(int32_t task_id,
                                          const LinquCoordinate& target,
                                          const std::string& kernel_so,
                                          LinquParam* params, int num_params)>;

class LinquOrchestratorState {
public:
    void init(Level level, const LinquCoordinate& coord,
              const LinquOrchConfig_Internal& cfg);
    void reset();

    void submit_task(LinquCoordinate_C target, const char* kernel_so,
                     LinquParam* params, int num_params);
    void scope_begin();
    void scope_end();
    uint64_t alloc_tensor(LinquCoordinate_C target, size_t size_bytes);
    void free_tensor(uint64_t handle);
    void orchestration_done();
    void wait_all();

    LinquCoordinate_C self_coord() const;
    Level level() const { return level_; }

    LinquRuntime* runtime() { return &rt_; }

    void set_dispatch_fn(TaskDispatchFn fn) { dispatch_fn_ = std::move(fn); }
    void set_dispatcher(LinquDispatcher* d);

    void set_peer_registry(PeerRegistry* reg) { peer_registry_ = reg; }
    void set_filesystem_discovery(FilesystemDiscovery* disc) { fs_discovery_ = disc; }
    PeerRegistry* peer_registry_ptr() const { return peer_registry_; }
    FilesystemDiscovery* fs_discovery_ptr() const { return fs_discovery_; }

    void on_task_complete(int32_t task_id);
    void on_scope_release(int32_t task_id);
    void try_advance_ring_pointers();

    LinquTaskRing& task_ring() { return task_ring_; }
    LinquHeapRing& heap_ring() { return heap_ring_; }
    LinquDepListPool& dep_pool() { return dep_pool_; }
    LinquTensorMap& tensor_map() { return tensor_map_; }
    LinquScopeStack& scope_stack() { return scope_stack_; }

    int64_t tasks_submitted() const { return tasks_submitted_; }

    RingMetrics current_metrics() const;
    ProfileReport generate_profile(const std::string& node_id) const;

    void take_snapshot(const std::string& label);
    const std::vector<RingSnapshot>& snapshots() const { return snapshots_; }

private:
    static LinquTaskDescriptor* scope_get_desc(int32_t tid, void* ctx);
    void wire_dispatcher_callback();
    void propagate_completion(int32_t task_id);
    void check_consumers_ready(int32_t producer_task_id);
    void check_task_consumed(int32_t task_id);

    Level level_ = Level::HOST;
    LinquCoordinate coord_;

    LinquTaskRing task_ring_;
    LinquHeapRing heap_ring_;
    LinquDepListPool dep_pool_;
    LinquTensorMap tensor_map_;
    LinquScopeStack scope_stack_;

    LinquRuntimeOps ops_;
    LinquRuntime rt_;

    TaskDispatchFn dispatch_fn_;
    LinquDispatcher* dispatcher_ = nullptr;
    PeerRegistry* peer_registry_ = nullptr;
    FilesystemDiscovery* fs_discovery_ = nullptr;

    std::mutex scheduler_mu_;

    int64_t tasks_submitted_ = 0;
    uint64_t next_tensor_handle_ = 1;
    bool done_ = false;

    size_t scope_begin_count_ = 0;
    size_t scope_end_count_ = 0;
    size_t free_tensor_count_ = 0;
    size_t task_retire_count_ = 0;
    size_t task_block_count_ = 0;
    size_t heap_block_count_ = 0;
    size_t peak_task_used_ = 0;
    size_t peak_heap_used_ = 0;
    std::vector<RingSnapshot> snapshots_;
};

}

#endif
