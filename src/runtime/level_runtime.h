#ifndef LINQU_RUNTIME_LEVEL_RUNTIME_H
#define LINQU_RUNTIME_LEVEL_RUNTIME_H

#include "core/tensor.h"
#include "ring/linqu_task_ring.h"
#include "ring/linqu_heap_ring.h"
#include "ring/linqu_dep_pool.h"
#include "runtime/linqu_tensormap.h"
#include "runtime/linqu_scope.h"
#include "profiling/trace_writer.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace linqu {

struct LevelRuntimeConfig {
    int32_t task_ring_window   = 4096;
    size_t  heap_ring_capacity = 64 * 1024 * 1024;  // 64 MiB
    int32_t dep_pool_capacity  = 8192;
    int32_t tensormap_buckets  = 4096;
    int32_t tensormap_pool     = 8192;
    int32_t ready_queue_capacity = 4096;
};

// =========================================================================
// LevelRuntime — CPU-side runtime for a single hierarchy level (L3–L7).
//
// Aligns with the simpler (L0-L2) runtime's architecture:
//   - LinquTaskRing       (fixed-window ring of task descriptors)
//   - LinquHeapRing       (ring allocator for output buffers)
//   - LinquDepListPool    (fanin/fanout linked-list chains)
//   - LinquTensorMap      (handle→producer for DAG dependency discovery)
//   - LinquScopeStack     (scope-based lifetime management)
//   - Ready queue          (single-type; simpler has per-worker-type)
//   - Event-driven completion propagation (fanin_refcount / fanout_refcount)
//
// Differences from simpler (all justified by L3-L7 vs L0-L2 environment):
//   - Workers execute std::function lambdas on CPU threads, not kernel_id
//     dispatched to AIC/AIV hardware cores.
//   - TensorMap uses handle exact-match (not multi-dim overlap detection).
//   - Single ready queue (only CPU worker type).
//   - Cross-level deps use shared atomic<bool> ready flag on LinquTensor
//     (simpler uses same-ring deps since all levels share one ring).
// =========================================================================

class LevelRuntime {
public:
    LevelRuntime(int level, int sched_threads, int worker_threads,
                 const LevelRuntimeConfig& cfg = {});
    ~LevelRuntime();

    LevelRuntime(const LevelRuntime&) = delete;
    LevelRuntime& operator=(const LevelRuntime&) = delete;

    void start();
    void stop();

    // Attach a shared TraceWriter for Perfetto trace generation.
    // Must be called BEFORE start().
    void set_trace_writer(TraceWriter* tw);

    // Register a trace "process" for a specific instance at this level.
    // Assigns process name + orchestrator / scheduler / worker thread names.
    // trace_pid must be a globally unique int32 (e.g. level*10000 + index).
    void register_trace_instance(int32_t trace_pid, const std::string& label);

    int level() const { return level_; }

    // make_tensor — metadata only; data_ptr remains nullptr.
    // Storage is allocated from HeapRing inside submit_worker().
    LinquTensor make_tensor(size_t count = 1);

    // submit_worker — register a task with tensor-typed DAG deps.
    //
    // `name` is stored in desc->kernel_so (same field simpler uses for the
    // kernel .so path).  The trace system reads it back as the task name.
    // `trace_pid` selects which instance's trace row gets this event
    // (-1 = use level_ as the default single-instance pid).
    //
    // Follows simpler's pto2_submit_task protocol:
    //   1. Back-pressure spin if TaskRing full
    //   2. Alloc task slot
    //   3. Build fanin chain from inputs via TensorMap lookup
    //   4. Alloc output buffers from HeapRing
    //   5. Register outputs in TensorMap
    //   6. fanin_count==0 → READY (push ready queue), else PENDING
    //
    // Returns future that resolves when fn() completes.
    std::future<void> submit_worker(
        const std::string&       name,
        std::function<void()>    fn,
        std::vector<LinquTensor> inputs,
        std::vector<LinquTensor> outputs,
        int32_t trace_pid = -1);

    // submit_orchestrator — enqueue an orchestration function.
    // `name` is recorded in the trace as the orchestrator job name.
    // `trace_pid` selects which instance's trace row gets this event.
    template <typename Fn>
    auto submit_orchestrator(const std::string& name, Fn&& fn,
                             int32_t trace_pid = -1)
        -> std::future<decltype(fn())>
    {
        using Ret = decltype(fn());
        auto task = std::make_shared<std::packaged_task<Ret()>>(
            std::forward<Fn>(fn));
        std::future<Ret> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(orch_mu_);
            orch_queue_.push({std::string(name), [task] { (*task)(); }, trace_pid});
        }
        orch_cv_.notify_one();
        return fut;
    }

private:
    // --- Simpler-aligned completion propagation ---
    void on_task_complete(int32_t task_id);
    void propagate_completion(int32_t task_id);
    void check_task_consumed(int32_t task_id);
    void try_advance_ring_pointers();

    // --- Simpler-aligned fanout spinlock ---
    static void fanout_lock(LinquTaskDescriptor* desc);
    static void fanout_unlock(LinquTaskDescriptor* desc);
    void add_consumer_to_producer(int32_t producer_id, int32_t consumer_id);

    // --- Ready queue (simpler-style circular buffer) ---
    void enqueue_ready(int32_t task_id);
    int32_t dequeue_ready();  // returns -1 if empty

    // --- Thread loops ---
    void orchestrator_loop();
    void scheduler_loop(int sched_idx);
    void worker_loop(int worker_idx);

    bool should_stop() const;

    // ---- Configuration ----
    int level_;
    int sched_count_;
    int worker_count_;

    // ---- Tracing (optional, null if disabled) ----
    TraceWriter* trace_ = nullptr;
    // Thread-id assignment within this level's "process" (pid = level_):
    //   tid 0          = orchestrator
    //   tid 1..S       = scheduler threads
    //   tid S+1..S+W   = worker threads
    int trace_tid_orch()             const { return 0; }
    int trace_tid_sched(int idx)     const { return 1 + idx; }
    int trace_tid_worker(int idx)    const { return 1 + sched_count_ + idx; }

    // ---- Simpler-aligned ring structures ----
    LinquTaskRing    task_ring_;
    LinquHeapRing    heap_ring_;
    LinquDepListPool dep_pool_;
    LinquTensorMap   tensor_map_;
    LinquScopeStack  scope_stack_;

    // ---- Ready queue (int32_t circular buffer, like simpler PTO2ReadyQueue) ----
    std::vector<int32_t> rq_buf_;
    int32_t rq_capacity_ = 0;
    std::atomic<int32_t> rq_head_{0};  // consumer (worker dequeue)
    std::atomic<int32_t> rq_tail_{0};  // producer (enqueue)

    // ---- Per-task promise for submit_worker future ----
    // Indexed by task_id % task_ring_window.
    std::vector<std::shared_ptr<std::promise<void>>> task_promises_;
    // Per-task output tensors (for mark_ready after fn completes).
    std::vector<std::vector<LinquTensor>> task_outputs_;
    // Per-task external (cross-level) input handles not found in TensorMap.
    std::vector<std::vector<TensorHandle>> task_external_inputs_;
    // Per-task external input tensor ready pointers (for scheduler polling).
    std::vector<std::vector<std::shared_ptr<std::atomic<bool>>>> task_external_ready_;
    // Per-task trace process id (-1 = use level_ as default).
    std::vector<int32_t> task_trace_pids_;

    // ---- Thread state ----
    mutable std::mutex state_mu_;
    bool running_        = false;
    bool stop_requested_ = false;

    // Orchestrator
    struct OrchJob {
        std::string name;
        std::function<void()> fn;
        int32_t trace_pid = -1;
    };
    std::thread                        orch_thread_;
    std::mutex                         orch_mu_;
    std::condition_variable            orch_cv_;
    std::queue<OrchJob>                orch_queue_;

    // Scheduler (cross-level readiness)
    std::vector<std::thread>           sched_threads_;
    std::mutex                         sched_mu_;
    std::condition_variable            sched_cv_;

    // Workers
    std::vector<std::thread>           worker_threads_;
    std::mutex                         worker_mu_;
    std::condition_variable            worker_cv_;
};

}

#endif
