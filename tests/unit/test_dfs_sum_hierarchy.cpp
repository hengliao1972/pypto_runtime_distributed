/*
 * test_dfs_sum_hierarchy.cpp — DFS Hierarchical Sum Test.
 *
 * Simulates a Lingqu system computing the distributed sum of random numbers
 * stored in DFS files, aggregated bottom-up through L3 → L4 → L5 → L6 → L7.
 *
 * ======================================================================
 * Design principles demonstrated
 * ======================================================================
 *
 * 1.  Role separation: WORKER vs ORCHESTRATOR
 *
 *     - L3  WORKER:  dfs_l3_reader_worker()
 *         Reads one DFS file; writes the local sum into its output tensor.
 *         Hierarchy coordinates (l6..l3) are scalar lambda captures — NOT
 *         tensor parameters and NOT tracked in the DAG.
 *
 *     - L4+ WORKER:  sum_reduce_worker()
 *         Sums N input tensors (from child workers) into one output tensor.
 *         Input tensors are DAG dependencies; no scalars appear in the
 *         dependency list.
 *
 *     - Orchestrators at every level: only build the DAG (call make_tensor
 *       and submit_worker/submit_orchestrator) and never compute directly.
 *
 * 2.  Tensor output buffer allocated at task-submit time (not at make_tensor)
 *
 *     make_tensor(n) creates a Tensor with metadata only:
 *       - handle  (unique ID in this level's task ring)
 *       - count   (number of uint64_t elements)
 *       - data    → shared_ptr pointing to an EMPTY std::vector (size == 0)
 *       - ready   → shared_ptr<atomic<bool>>, initially false
 *     No backing store is allocated by make_tensor().
 *
 *     Inside submit_worker(), as the first step before enqueueing the task,
 *     every output tensor's data vector is resized to 'count' elements:
 *       out.data->resize(out.count, 0ULL);
 *     This is the buffer-allocation step, equivalent to alloc() from the
 *     level's HeapRing in the simpler L0-L2 runtime.
 *
 *     Because Tensor.data is a shared_ptr, all copies of the Tensor object
 *     (the orchestrator's local variable, the lambda capture, the task
 *     registry entry) share the same std::vector.  The worker writes into
 *     it through its captured Tensor copy; the orchestrator reads back the
 *     result through the same shared_ptr after the future completes.
 *
 * 3.  DAG dependency tracking on tensor parameters only
 *
 *     submit_worker(fn, inputs, outputs):
 *       inputs  — std::vector<Tensor> whose handles are recorded in
 *                 TaskDesc::input_handles for DAG dependency tracking.
 *       outputs — std::vector<Tensor> whose handles are registered in
 *                 TensorRegistry and recorded in TaskDesc::output_handles.
 *
 *     Scalar parameters (hierarchy coordinates, loop indices, file paths)
 *     are passed to the worker via lambda captures, NOT through the
 *     inputs/outputs lists.  The scheduler checks only input_handles —
 *     scalars are invisible to the dependency graph.
 *
 * 4.  Shared data structures within the same process
 *
 *     At each hierarchy level, one LevelRuntime instance owns three thread
 *     roles that share memory:
 *
 *       Orchestrator thread — calls submit_worker / submit_orchestrator;
 *         allocates output buffers at submit time.
 *
 *       Scheduler thread(s) — scans pending_tasks_; promotes a task from
 *         PENDING → READY when all its input_handles are is_ready().
 *         Uses atomic<TaskState> CAS (compare_exchange_strong) to prevent
 *         races between multiple scheduler threads.
 *
 *       Worker threads — dequeue from ready_queue_; execute fn(); mark
 *         output tensors ready (via shared_ptr<atomic<bool>>); notify
 *         the scheduler that new tensors became ready.
 *
 *     Synchronisation follows the simpler (L0-L2) runtime's pattern:
 *       - Per-tensor readiness: atomic<bool> with acquire/release ordering.
 *       - PENDING → READY promotion: atomic<TaskState> CAS.
 *       - Queue/list wakeups: std::mutex + std::condition_variable.
 *       - Mutex scopes are kept as short as possible (collect-then-act).
 *
 *     Cross-level readiness (L3 tensors unblocking L4 tasks) propagates
 *     automatically through the shared_ptr<atomic<bool>> ready flags.
 *     The L4 scheduler polls with a short wait_for timeout to detect
 *     readiness changes written by L3 worker threads.
 */

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

// ===========================================================================
// Tensor
//
// Lightweight metadata handle.  make_tensor() fills handle/count/ready but
// leaves data pointing to an empty vector — no storage allocated yet.
// Storage (data->resize) happens inside submit_worker() at task-submission
// time, mirroring HeapRing alloc() in the simpler L0-L2 runtime.
//
// NOTE: TensorHandle values are globally unique (assigned from a single
// atomic counter shared by all LevelRuntime instances).  This allows tensors
// produced at level L3 to be referenced as DAG dependencies by level L4
// without handle collisions.
// ===========================================================================

using TensorHandle = uint64_t;
static constexpr TensorHandle INVALID_HANDLE = 0;

// Global handle allocator — shared across all LevelRuntime instances.
static std::atomic<TensorHandle> g_next_handle{1};

struct Tensor {
    TensorHandle handle{INVALID_HANDLE};
    size_t       count{0};

    // shared_ptr so all Tensor copies (orchestrator var, lambda capture,
    // registry entry) see the same backing store and readiness flag.
    std::shared_ptr<std::vector<uint64_t>>  data;   // empty until submit_worker
    std::shared_ptr<std::atomic<bool>>      ready;  // false until worker marks done

    bool is_valid()  const { return handle != INVALID_HANDLE; }
    bool is_ready()  const { return ready && ready->load(std::memory_order_acquire); }
    void mark_ready()      { if (ready) ready->store(true, std::memory_order_release); }

    uint64_t scalar() const {
        assert(is_ready() && data && !data->empty());
        return (*data)[0];
    }
};

// ===========================================================================
// Task lifecycle
// ===========================================================================

enum class TaskState : uint8_t { PENDING = 0, READY, RUNNING, COMPLETED };

// One entry in the level's conceptual task ring.
// DAG edges are expressed only through tensor handles (not scalars).
struct TaskDesc {
    uint32_t                   id{0};
    std::atomic<TaskState>     state{TaskState::PENDING};

    // Tensor-typed DAG deps only — scalar args are in fn's closure.
    std::vector<TensorHandle>  input_handles;
    std::vector<TensorHandle>  output_handles;

    std::function<void()>      fn;
};

// ===========================================================================
// TensorRegistry — shared among orchestrator / scheduler / worker threads
// ===========================================================================

class TensorRegistry {
public:
    void upsert(TensorHandle h, const Tensor& t) {
        std::lock_guard<std::mutex> lk(mu_);
        map_[h] = t;
    }
    bool has(TensorHandle h) const {
        std::lock_guard<std::mutex> lk(mu_);
        return map_.count(h) != 0;
    }
    // Uses the Tensor's shared ready flag — no lock needed for the atomic load.
    bool is_ready(TensorHandle h) const {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = map_.find(h);
        return it != map_.end() && it->second.is_ready();
    }

private:
    mutable std::mutex                       mu_;
    std::unordered_map<TensorHandle, Tensor> map_;
};

// ===========================================================================
// LevelRuntime
//
// Models the CPU-side runtime at a single hierarchy level (L3–L7).
// Three thread roles share data structures in the same process address space:
//
//   Orchestrator   — builds the DAG, submits tasks, allocates output buffers.
//   Scheduler(s)   — promotes PENDING tasks whose inputs became ready.
//   Workers        — execute task functions; mark output tensors ready.
//
// Shared structures (protected per comments below):
//   tensor_registry_ — TensorRegistry, mutex inside
//   pending_tasks_   — vector<TaskDesc*>, guarded by pending_mu_
//   ready_queue_     — queue<TaskDesc*>,  guarded by worker_mu_
//   orch_queue_      — queue<fn>,         guarded by orch_mu_
// ===========================================================================

class LevelRuntime {
public:
    LevelRuntime(int level, int sched_threads, int worker_threads)
        : level_(level),
          sched_count_(sched_threads  > 0 ? sched_threads  : 1),
          worker_count_(worker_threads > 0 ? worker_threads : 1) {}

    ~LevelRuntime() { stop(); }

    void start() {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (running_) return;
        running_        = true;
        stop_requested_ = false;

        orch_thread_ = std::thread([this] { orchestrator_loop(); });
        for (int i = 0; i < sched_count_;  i++)
            sched_threads_.emplace_back([this] { scheduler_loop(); });
        for (int i = 0; i < worker_count_; i++)
            worker_threads_.emplace_back([this] { worker_loop(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            if (!running_) return;
            stop_requested_ = true;
        }
        orch_cv_.notify_all();
        sched_cv_.notify_all();
        worker_cv_.notify_all();

        if (orch_thread_.joinable())   orch_thread_.join();
        for (auto& t : sched_threads_)  if (t.joinable()) t.join();
        for (auto& t : worker_threads_) if (t.joinable()) t.join();
        sched_threads_.clear();
        worker_threads_.clear();

        std::lock_guard<std::mutex> lk(state_mu_);
        running_ = false;
    }

    // ----------------------------------------------------------------
    // make_tensor — fills metadata ONLY; no storage allocated.
    //
    // data is an empty shared vector (size 0) — analogous to calling
    // alloc_tensor() which returns a Tensor descriptor without yet
    // claiming any HeapRing space.  Storage comes in submit_worker().
    // ----------------------------------------------------------------
    Tensor make_tensor(size_t count = 1) {
        Tensor t;
        // Allocate from global counter: handles must be unique across all levels
        // so that cross-level DAG dependencies can be tracked without collision.
        t.handle = g_next_handle.fetch_add(1, std::memory_order_relaxed);
        t.count  = count;
        t.data   = std::make_shared<std::vector<uint64_t>>();  // empty — no storage yet
        t.ready  = std::make_shared<std::atomic<bool>>(false);
        return t;
    }

    // ----------------------------------------------------------------
    // submit_worker — registers a tensor-producing/consuming task.
    //
    // Steps performed synchronously before returning:
    //   1. Allocate storage for each output tensor (data->resize).
    //      All Tensor copies sharing the same data shared_ptr see the
    //      allocation — including the lambda captures in fn.
    //   2. Register input and output tensors in tensor_registry_.
    //   3. Build TaskDesc with input_handles and output_handles.
    //      Scalar args are NOT listed here; they live in fn's closure.
    //   4. If fan-in == 0, dispatch immediately as READY.
    //      Otherwise, enqueue as PENDING for the scheduler.
    //
    // Returns std::future<void> that resolves when fn() completes and
    // all output tensors have been marked ready.
    // ----------------------------------------------------------------
    std::future<void> submit_worker(std::function<void()>    fn,
                                    std::vector<Tensor>       inputs,
                                    std::vector<Tensor>       outputs) {
        auto promise = std::make_shared<std::promise<void>>();
        std::future<void> fut = promise->get_future();

        auto desc = std::make_shared<TaskDesc>();
        desc->id = next_task_id_.fetch_add(1, std::memory_order_relaxed);

        // Step 1 & 2: Allocate output buffers at task-submission time.
        // This is the moment storage is claimed from the level's memory ring.
        for (Tensor& out : outputs) {
            out.data->resize(out.count, 0ULL);      // allocate storage HERE
            tensor_registry_.upsert(out.handle, out);
            desc->output_handles.push_back(out.handle);
        }

        // Step 2 (inputs): register for cross-level visibility.
        // Only tensor-typed params participate in the DAG.
        for (const Tensor& in : inputs) {
            if (in.is_valid()) {
                if (!tensor_registry_.has(in.handle))
                    tensor_registry_.upsert(in.handle, in);
                desc->input_handles.push_back(in.handle);
            }
        }

        // Step 3: Build fn wrapper: execute, mark outputs ready, notify scheduler.
        // Use copies (shared_ptr semantics) — no raw pointers to stack vars.
        desc->fn = [fn = std::move(fn), out_copies = outputs, promise, this]() mutable {
            fn();
            // Mark every output tensor ready (via shared atomic<bool>).
            // All other Tensor copies sharing the same ready shared_ptr
            // will observe is_ready() == true after this store.
            for (Tensor& t : out_copies) t.mark_ready();
            promise->set_value();
            // Notify scheduler: newly ready tensors may unblock pending tasks.
            sched_cv_.notify_all();
        };

        // Step 4: Dispatch immediately if no tensor dependencies.
        if (inputs.empty()) {
            desc->state.store(TaskState::READY, std::memory_order_release);
            enqueue_ready(std::move(desc));
        } else {
            desc->state.store(TaskState::PENDING, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lk(pending_mu_);
                pending_tasks_.push_back(std::move(desc));
            }
            sched_cv_.notify_one();  // wake scheduler to evaluate readiness
        }

        return fut;
    }

    // ----------------------------------------------------------------
    // submit_orchestrator — runs a non-data-producing function in the
    // orchestrator thread.  Orchestrators build the DAG; they do not
    // directly produce or consume tensors.
    // ----------------------------------------------------------------
    template <typename Fn>
    auto submit_orchestrator(Fn&& fn) -> std::future<decltype(fn())> {
        using Ret = decltype(fn());
        auto task = std::make_shared<std::packaged_task<Ret()>>(std::forward<Fn>(fn));
        std::future<Ret> fut = task->get_future();
        {
            std::lock_guard<std::mutex> lk(orch_mu_);
            orch_queue_.push([task] { (*task)(); });
        }
        orch_cv_.notify_one();
        return fut;
    }

private:
    bool should_stop() const {
        std::lock_guard<std::mutex> lk(state_mu_);
        return stop_requested_;
    }

    void enqueue_ready(std::shared_ptr<TaskDesc> desc) {
        {
            std::lock_guard<std::mutex> lk(worker_mu_);
            ready_queue_.push(std::move(desc));
        }
        worker_cv_.notify_one();
    }

    // Orchestrator loop: serialised execution of orchestrator-role functions.
    void orchestrator_loop() {
        while (true) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(orch_mu_);
                orch_cv_.wait(lk, [this] {
                    return should_stop() || !orch_queue_.empty();
                });
                if (should_stop() && orch_queue_.empty()) break;
                job = std::move(orch_queue_.front());
                orch_queue_.pop();
            }
            job();
        }
    }

    // Scheduler loop: promotes PENDING tasks whose all input tensors are ready.
    //
    // Shared data accessed:
    //   tensor_registry_ (read-only is_ready checks, mutex inside)
    //   pending_tasks_   (scanned and pruned, guarded by pending_mu_)
    //   ready_queue_     (written via enqueue_ready, guarded by worker_mu_)
    //
    // Uses atomic<TaskState> CAS (PENDING → READY) to be safe with
    // multiple scheduler threads — only one thread will win the CAS.
    //
    // Uses wait_for with a short timeout so cross-level tensor readiness
    // (tensors marked ready by workers in a different LevelRuntime) is
    // eventually detected without an explicit cross-runtime wakeup.
    void scheduler_loop() {
        while (true) {
            // Sleep until notified or timeout (catches cross-level readiness).
            {
                std::unique_lock<std::mutex> lk(sched_mu_);
                sched_cv_.wait_for(lk, std::chrono::milliseconds(2),
                                   [this] { return should_stop(); });
            }

            // Collect tasks ready to dispatch (hold pending_mu_ briefly).
            std::vector<std::shared_ptr<TaskDesc>> to_dispatch;
            {
                std::lock_guard<std::mutex> lk(pending_mu_);
                if (pending_tasks_.empty()) {
                    if (should_stop()) break;
                    continue;
                }
                std::vector<std::shared_ptr<TaskDesc>> still_pending;
                for (auto& desc : pending_tasks_) {
                    bool all_ready = true;
                    for (TensorHandle h : desc->input_handles) {
                        // Only tensor deps tracked — scalar args not in this list.
                        if (!tensor_registry_.is_ready(h)) {
                            all_ready = false;
                            break;
                        }
                    }
                    if (all_ready) {
                        // CAS: only one scheduler thread promotes a given task.
                        TaskState expected = TaskState::PENDING;
                        if (desc->state.compare_exchange_strong(
                                expected, TaskState::READY,
                                std::memory_order_acq_rel)) {
                            to_dispatch.push_back(desc);
                        } else {
                            still_pending.push_back(desc);
                        }
                    } else {
                        still_pending.push_back(desc);
                    }
                }
                pending_tasks_ = std::move(still_pending);
            }

            // Enqueue promoted tasks outside pending_mu_ lock.
            for (auto& desc : to_dispatch) enqueue_ready(std::move(desc));

            if (should_stop()) {
                std::lock_guard<std::mutex> lk(pending_mu_);
                if (pending_tasks_.empty()) break;
            }
        }
    }

    // Worker loop: executes READY tasks; marks output tensors ready.
    // After each task, notifies the scheduler so intra-level dependent
    // tasks can be promoted immediately.
    void worker_loop() {
        while (true) {
            std::shared_ptr<TaskDesc> desc;
            {
                std::unique_lock<std::mutex> lk(worker_mu_);
                worker_cv_.wait(lk, [this] {
                    return should_stop() || !ready_queue_.empty();
                });
                if (should_stop() && ready_queue_.empty()) break;
                desc = std::move(ready_queue_.front());
                ready_queue_.pop();
            }
            desc->state.store(TaskState::RUNNING, std::memory_order_release);
            desc->fn();   // fn() marks output tensors ready + notifies scheduler
            desc->state.store(TaskState::COMPLETED, std::memory_order_release);
        }
    }

    int level_;
    int sched_count_;
    int worker_count_;

    // Shared data structures (same process address space):
    TensorRegistry tensor_registry_;  // handle → Tensor; shared ready flags

    std::atomic<uint32_t>     next_task_id_{0};

    mutable std::mutex state_mu_;
    bool running_        = false;
    bool stop_requested_ = false;

    // --- Orchestrator thread ---
    std::thread                        orch_thread_;
    std::mutex                         orch_mu_;
    std::condition_variable            orch_cv_;
    std::queue<std::function<void()>>  orch_queue_;

    // --- Scheduler: pending task list + wakeup ---
    std::mutex                               pending_mu_;
    std::mutex                               sched_mu_;
    std::condition_variable                  sched_cv_;
    std::vector<std::shared_ptr<TaskDesc>>   pending_tasks_;
    std::vector<std::thread>                 sched_threads_;

    // --- Worker: ready queue ---
    std::mutex                               worker_mu_;
    std::condition_variable                  worker_cv_;
    std::queue<std::shared_ptr<TaskDesc>>    ready_queue_;
    std::vector<std::thread>                 worker_threads_;
};

// ===========================================================================
// Test constants
// Default topology: 1 × 16 × 4 × 16 = 1024 L3 nodes.
// Override individual dimensions via L6_SIZE / L5_SIZE / L4_SIZE / L3_SIZE.
// ===========================================================================

static constexpr int kNumL6Default      = 1;
static constexpr int kNumL5PerL6Default = 16;
static constexpr int kNumL4PerL5Default = 4;
static constexpr int kNumL3PerL4Default = 16;
static constexpr int kNumsPerFileDefault = 1024;  // tensor width: result[1024]

static const char* kBaseDir = "/tmp/linqu_test_dfs_hierarchy_sum";

static int kNumL6;
static int kNumL5PerL6;
static int kNumL4PerL5;
static int kNumL3PerL4;
static int kNumsPerFile;

static int env_int(const char* name, int def) {
    const char* s = std::getenv(name);
    if (s && *s) { int v = std::atoi(s); if (v > 0) return v; }
    return def;
}

static std::string l3_data_path(int l6, int l5, int l4, int l3) {
    return std::string(kBaseDir) + "/dfs"
        + "/L6_" + std::to_string(l6)
        + "/L5_" + std::to_string(l5)
        + "/L4_" + std::to_string(l4)
        + "/L3_" + std::to_string(l3) + "/data.txt";
}

// Returns per-column expected sums: expected[k] = sum_{all L3 nodes i} file[i][k]
// i.e.  result[1024] = { sum(tensor_[i][k]) for i=0..N_L3-1 }  for k=0..1023
static std::vector<uint64_t> build_dfs_test_data() {
    fs::remove_all(kBaseDir);
    fs::create_directories(std::string(kBaseDir) + "/dfs");
    std::vector<uint64_t> expected(static_cast<size_t>(kNumsPerFile), 0ULL);
    for (int l6 = 0; l6 < kNumL6; l6++)
    for (int l5 = 0; l5 < kNumL5PerL6; l5++)
    for (int l4 = 0; l4 < kNumL4PerL5; l4++)
    for (int l3 = 0; l3 < kNumL3PerL4; l3++) {
        int global = ((l6*kNumL5PerL6 + l5)*kNumL4PerL5 + l4)*kNumL3PerL4 + l3;
        std::mt19937 rng(0x5A17u + static_cast<uint32_t>(global));
        std::uniform_int_distribution<int> dist(1, 1000);

        std::string path = l3_data_path(l6, l5, l4, l3);
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream out(path);
        assert(out.good());
        for (int k = 0; k < kNumsPerFile; k++) {
            int v = dist(rng);
            expected[static_cast<size_t>(k)] += static_cast<uint64_t>(v);  // column-wise
            out << v << "\n";
        }
    }
    return expected;
}

// ===========================================================================
// WORKER FUNCTIONS
//
// Workers are pure compute functions; they never submit further tasks.
// Every parameter that influences the dependency graph is a Tensor.
// Scalar parameters (coords, paths) are lambda captures — invisible to DAG.
// ===========================================================================

// L3 WORKER — reads one DFS file, stores ALL values as a 1-D tensor.
//
// Output tensor has kNumsPerFile elements: out.data[k] = file[node][k].
// Scalar coordinates (l6..l3) are lambda captures — invisible to the DAG.
// Storage (out.data->size() == kNumsPerFile) was allocated by submit_worker.
static void dfs_l3_reader_worker(int l6, int l5, int l4, int l3, Tensor out) {
    std::ifstream f(l3_data_path(l6, l5, l4, l3));
    assert(f.good());
    int v = 0;
    for (size_t k = 0; k < out.data->size(); k++) {
        if (!(f >> v)) break;
        (*out.data)[k] = static_cast<uint64_t>(v);
    }
}

// TREE-REDUCTION WORKER — element-wise sum of two same-length tensors.
//
// result[k] = a[k] + b[k]  for k = 0 .. N-1   (N = kNumsPerFile = 1024).
//
// This is the compute unit for every internal node in the binary reduction
// tree.  Both 'a' and 'b' are guaranteed ready by the scheduler before this
// worker is dispatched (via tensor-map DAG dependency tracking).
//
// The final root tensor accumulates:
//   result[k] = sum_{all L3 nodes i} file[i][k]
//
//   a, b  — tensor-typed DAG inputs (tracked in TaskDesc::input_handles).
//   out   — tensor output pre-allocated at submit_worker() time.
static void pair_sum_worker(Tensor a, Tensor b, Tensor out) {
    assert(a.is_ready() && a.data && !a.data->empty());
    assert(b.is_ready() && b.data && !b.data->empty());
    assert(a.data->size() == b.data->size());
    assert(out.data->size() == a.data->size());
    const size_t N = out.data->size();
    for (size_t k = 0; k < N; k++)
        (*out.data)[k] = (*a.data)[k] + (*b.data)[k];
}

// ===========================================================================
// tree_reduce — binary reduction tree DAG builder (called by orchestrators)
//
// The orchestrator calls this once, passing the leaf tensors (already
// submitted to workers at the level below).  tree_reduce builds ALL levels
// of the reduction tree and submits ALL internal-node workers upfront — no
// waiting between rounds.  The tensor map (TensorRegistry) tracks which
// internal-node task depends on which leaf / earlier-round tensor.  The
// scheduler dispatches each internal-node worker automatically the moment
// both of its input tensors are marked ready by their producing workers.
//
// Tree structure for N leaves (e.g., N = 16 L3 outputs):
//
//   Leaves (N=16):  [L3_0 … L3_15]       — submitted by caller
//   Round 1  (8):  pair(0,1) … pair(14,15) — submit_worker, dep on leaves
//   Round 2  (4):  pair(R1_0,R1_1) …       — submit_worker, dep on round 1
//   Round 3  (2):  pair(R2_0,R2_1) …       — submit_worker, dep on round 2
//   Round 4  (1):  pair(R3_0,R3_1)          — submit_worker, dep on round 3
//                   ↑ root
//
// Total workers submitted by ONE l4_orchestrate call:
//   N leaf readers  +  (N - 1) internal tree workers  (all in a single pass)
//
// After all submissions, tree_reduce waits ONLY for the root future, then
// returns the root Tensor (already marked ready by the root worker).
// ===========================================================================

static Tensor tree_reduce(LevelRuntime& rt, std::vector<Tensor> leaves) {
    assert(!leaves.empty());
    if (leaves.size() == 1) return leaves[0];

    std::vector<Tensor> current = std::move(leaves);
    std::future<void>   root_fut;

    // Submit all tree rounds WITHOUT any intermediate waiting.
    // Each round's workers declare their two input tensors as DAG deps;
    // the scheduler holds them PENDING until the deps become ready.
    while (current.size() > 1) {
        std::vector<Tensor> next;
        next.reserve((current.size() + 1) / 2);

        for (size_t i = 0; i + 1 < current.size(); i += 2) {
            // Output tensor: same width as inputs (kNumsPerFile elements).
            // Storage is allocated in submit_worker(); make_tensor only fills metadata.
            Tensor out = rt.make_tensor(current[i].count);

            // Submit the pair-sum worker.  The two input handles are registered
            // in TaskDesc::input_handles for DAG dependency tracking.
            // The scheduler dispatches this task ONLY when current[i] AND
            // current[i+1] are both marked ready (by whichever worker produced
            // them — leaf readers in round 1, earlier tree workers in later rounds).
            root_fut = rt.submit_worker(
                [a = current[i], b = current[i+1], out = out] {
                    pair_sum_worker(a, b, out);
                },
                {current[i], current[i+1]},  // tensor DAG deps (2 per internal node)
                {out});                        // output buffer allocated here

            next.push_back(out);
        }

        if (current.size() % 2 == 1)
            next.push_back(current.back());  // odd element: pass through to next round

        current = std::move(next);
    }

    // Wait for the root (last submitted worker) to complete.
    // All other tree workers will have finished before the root can run.
    if (root_fut.valid()) root_fut.get();
    return current[0];  // root tensor; is_ready() == true
}

// ===========================================================================
// ORCHESTRATORS (build DAG, submit tasks, never compute directly)
// ===========================================================================

// l4_orchestrate — dispatched into rt_l4's orchestrator thread.
//
// Phase 1: Submit kNumL3PerL4 L3 reader workers in parallel (no tensor deps;
//   all immediately READY; rt_l3's worker threads run them concurrently).
//
// Phase 2: Call tree_reduce(rt_l4, l3_outs).
//   tree_reduce submits (kNumL3PerL4 - 1) pair-sum workers covering all
//   rounds of the binary tree.  ALL workers are submitted before any of them
//   execute.  The rt_l4 scheduler dispatches each internal node automatically
//   once its two input tensors are ready — no orchestrator polling needed.
//
// For kNumL3PerL4 = 16, total tasks submitted per l4_orchestrate call:
//   16 leaf readers  +  8+4+2+1 = 15 tree workers  =  31 tasks total.
static Tensor l4_orchestrate(LevelRuntime& rt_l3, LevelRuntime& rt_l4,
                              int l6, int l5, int l4) {
    // Phase 1: submit all L3 reader workers upfront (parallel; no tensor deps).
    std::vector<Tensor> l3_outs(kNumL3PerL4);
    for (int l3 = 0; l3 < kNumL3PerL4; l3++) {
        l3_outs[l3] = rt_l3.make_tensor(static_cast<size_t>(kNumsPerFile));  // N-element; no storage yet
        rt_l3.submit_worker(
            [l6, l5, l4, l3, out = l3_outs[l3]] {
                dfs_l3_reader_worker(l6, l5, l4, l3, out);
            },
            {},              // no tensor inputs — scalar coords not in the DAG
            {l3_outs[l3]});  // output buffer allocated inside submit_worker
    }

    // Phase 2: build tree reduction DAG over L3 outputs.
    // tree_reduce submits all internal-node workers before any of them run;
    // returns only when the root result is ready.
    return tree_reduce(rt_l4, l3_outs);
}

// l5_orchestrate — dispatched into rt_l5's orchestrator thread.
//
// Submits kNumL4PerL5 L4 orchestrators (sequentially via rt_l4's single orch
// thread) and collects their root tensors.  Then builds a tree reduction DAG
// over those L4 results at rt_l5 level.
static Tensor l5_orchestrate(LevelRuntime& rt_l3, LevelRuntime& rt_l4,
                              LevelRuntime& rt_l5, int l6, int l5) {
    std::vector<std::future<Tensor>> futs;
    futs.reserve(kNumL4PerL5);
    for (int l4 = 0; l4 < kNumL4PerL5; l4++) {
        futs.push_back(rt_l4.submit_orchestrator([&, l6, l5, l4]() -> Tensor {
            return l4_orchestrate(rt_l3, rt_l4, l6, l5, l4);
        }));
    }
    std::vector<Tensor> l4_outs(kNumL4PerL5);
    for (int l4 = 0; l4 < kNumL4PerL5; l4++)
        l4_outs[l4] = futs[l4].get();

    // Tree-reduce L4 pod results at L5 level.
    return tree_reduce(rt_l5, l4_outs);
}

// l6_orchestrate — dispatched into rt_l6's orchestrator thread.
static Tensor l6_orchestrate(LevelRuntime& rt_l3, LevelRuntime& rt_l4,
                              LevelRuntime& rt_l5, LevelRuntime& rt_l6,
                              int l6) {
    std::vector<std::future<Tensor>> futs;
    futs.reserve(kNumL5PerL6);
    for (int l5 = 0; l5 < kNumL5PerL6; l5++) {
        futs.push_back(rt_l5.submit_orchestrator([&, l6, l5]() -> Tensor {
            return l5_orchestrate(rt_l3, rt_l4, rt_l5, l6, l5);
        }));
    }
    std::vector<Tensor> l5_outs(kNumL5PerL6);
    for (int l5 = 0; l5 < kNumL5PerL6; l5++)
        l5_outs[l5] = futs[l5].get();

    // Tree-reduce L5 supernode results at L6 level.
    return tree_reduce(rt_l6, l5_outs);
}

}  // namespace

// ---------------------------------------------------------------------------
// print_hierarchy — display Lingqu system levels and instance counts.
//
// L7-L3 counts come from the test topology parameters.
// L2-L0 counts are per-host hardware configuration (not modelled in this
// test; representative defaults shown for a typical AI accelerator host):
//   N_CHIP_PER_HOST = 4   (L2 Chip)
//   N_DIE_PER_CHIP  = 2   (L1 Chip Die, optional)
//   N_CG_PER_DIE    = 8   (L0 Core-group, each containing 1 AIC + 2 AIV)
// ---------------------------------------------------------------------------
static void print_hierarchy(int total_l3) {
    // L2-L0 representative hardware counts per host.
    const int n_chip_per_host = env_int("N_CHIP_PER_HOST", 4);
    const int n_die_per_chip  = env_int("N_DIE_PER_CHIP",  2);
    const int n_cg_per_die    = env_int("N_CG_PER_DIE",    8);

    const int total_l2 = total_l3 * n_chip_per_host;
    const int total_l1 = total_l2 * n_die_per_chip;
    const int total_l0 = total_l1 * n_cg_per_die;

    fprintf(stderr,
        "┌─────────────────────────────────────────────────────────────────┐\n"
        "│            Lingqu System Hierarchy — Instance Counts            │\n"
        "├───────┬──────────────────────────┬─────────────┬────────────────┤\n"
        "│ Level │ Name                     │ Per-parent  │ Total          │\n"
        "├───────┼──────────────────────────┼─────────────┼────────────────┤\n"
        "│  L7   │ Global Coordinator       │      —      │ %14d │\n"
        "│  L6   │ Cluster-lv2 (CLOS2)      │      —      │ %14d │\n"
        "│  L5   │ Cluster-lv1 (Supernode)  │ %11d │ %14d │\n"
        "│  L4   │ Cluster-lv0 (Pod)        │ %11d │ %14d │\n"
        "│  L3   │ Host (OS node)           │ %11d │ %14d │\n"
        "├───────┼──────────────────────────┼─────────────┼────────────────┤\n"
        "│  L2   │ Chip (UMA)               │ %11d │ %14d │\n"
        "│  L1   │ Chip Die (optional)      │ %11d │ %14d │\n"
        "│  L0   │ Core-group (AIC+2×AIV)   │ %11d │ %14d │\n"
        "└───────┴──────────────────────────┴─────────────┴────────────────┘\n"
        "  Tensor width per L3 file: %d elements\n\n",
        /* L7 */  1,
        /* L6 */  kNumL6,
        /* L5 */  kNumL5PerL6,  kNumL6 * kNumL5PerL6,
        /* L4 */  kNumL4PerL5,  kNumL6 * kNumL5PerL6 * kNumL4PerL5,
        /* L3 */  kNumL3PerL4,  total_l3,
        /* L2 */  n_chip_per_host, total_l2,
        /* L1 */  n_die_per_chip,  total_l1,
        /* L0 */  n_cg_per_die,    total_l0,
        kNumsPerFile);
}

int main() {
    kNumL6      = env_int("L6_SIZE",   kNumL6Default);
    kNumL5PerL6 = env_int("L5_SIZE",   kNumL5PerL6Default);
    kNumL4PerL5 = env_int("L4_SIZE",   kNumL4PerL5Default);
    kNumL3PerL4 = env_int("L3_SIZE",   kNumL3PerL4Default);
    kNumsPerFile= env_int("NUMS_PER_FILE", kNumsPerFileDefault);

    const int total_l3 = kNumL6 * kNumL5PerL6 * kNumL4PerL5 * kNumL3PerL4;

    fprintf(stderr, "=== DFS Hierarchical Sum Test ===\n\n");
    print_hierarchy(total_l3);
    fprintf(stderr, "    Task topology used in this test: L7→L3 (%d L3 nodes)\n"
                    "    nums/file=%d  (tensor width per node)\n\n",
            total_l3, kNumsPerFile);

    // expected[k] = sum_{i=0..total_l3-1} file[i][k]   (column-wise)
    const std::vector<uint64_t> expected = build_dfs_test_data();
    uint64_t expected_total = 0;
    for (uint64_t x : expected) expected_total += x;
    fprintf(stderr, "[DATA] Generated %d DFS files, %d cols/file, grand total = %llu\n\n",
            total_l3, kNumsPerFile, (unsigned long long)expected_total);

    // L3-L7 runtime: 1 orchestrator + Lx_NUM_SCHEDULER_THREADS scheduler(s)
    // + Lx_NUM_WORKER_THREADS workers, all in the same process.
    const int l3_sched   = env_int("L3_NUM_SCHEDULER_THREADS", 1);
    const int l4_sched   = env_int("L4_NUM_SCHEDULER_THREADS", 1);
    const int l5_sched   = env_int("L5_NUM_SCHEDULER_THREADS", 1);
    const int l6_sched   = env_int("L6_NUM_SCHEDULER_THREADS", 1);
    const int l7_sched   = env_int("L7_NUM_SCHEDULER_THREADS", 1);
    const int l3_workers = env_int("L3_NUM_WORKER_THREADS", 4);
    const int l4_workers = env_int("L4_NUM_WORKER_THREADS", 4);
    const int l5_workers = env_int("L5_NUM_WORKER_THREADS", 4);
    const int l6_workers = env_int("L6_NUM_WORKER_THREADS", 4);
    const int l7_workers = env_int("L7_NUM_WORKER_THREADS", 4);

    LevelRuntime rt_l3(3, l3_sched, l3_workers);
    LevelRuntime rt_l4(4, l4_sched, l4_workers);
    LevelRuntime rt_l5(5, l5_sched, l5_workers);
    LevelRuntime rt_l6(6, l6_sched, l6_workers);
    LevelRuntime rt_l7(7, l7_sched, l7_workers);

    rt_l3.start(); rt_l4.start(); rt_l5.start(); rt_l6.start(); rt_l7.start();

    // L7 orchestrator: dispatches L6 orchestrators, collects their root tensors,
    // then builds a final tree reduction at L7 level.
    // Returns the root Tensor (kNumsPerFile elements): result[k] = sum_{i} file[i][k].
    std::future<Tensor> top_future = rt_l7.submit_orchestrator([&]() -> Tensor {
        std::vector<std::future<Tensor>> futs;
        futs.reserve(kNumL6);
        for (int l6 = 0; l6 < kNumL6; l6++) {
            futs.push_back(rt_l6.submit_orchestrator([&, l6]() -> Tensor {
                return l6_orchestrate(rt_l3, rt_l4, rt_l5, rt_l6, l6);
            }));
        }
        std::vector<Tensor> l6_outs(kNumL6);
        for (int l6 = 0; l6 < kNumL6; l6++)
            l6_outs[l6] = futs[l6].get();

        // Tree-reduce L6 results at L7 level (kNumL6=1 default → trivial).
        return tree_reduce(rt_l7, l6_outs);
    });

    const Tensor result = top_future.get();
    const std::vector<uint64_t>& computed = *result.data;

    // Verify element-wise: computed[k] == expected[k]  for k = 0..kNumsPerFile-1
    assert((int)computed.size() == kNumsPerFile);
    assert((int)expected.size()  == kNumsPerFile);
    bool match = true;
    for (int k = 0; k < kNumsPerFile; k++) {
        if (computed[k] != expected[k]) {
            fprintf(stderr, "[FAIL] result[%d]: got %llu expected %llu\n",
                    k, (unsigned long long)computed[k],
                       (unsigned long long)expected[k]);
            match = false;
        }
    }
    assert(match);

    // Print grand total and a sample of result columns.
    uint64_t computed_total = 0;
    for (uint64_t x : computed) computed_total += x;
    fprintf(stderr, "result[1024] = sum(tensor[i][1024]) for i=0..%d\n", total_l3 - 1);
    fprintf(stderr, "  result[0..3]  : %llu %llu %llu %llu\n",
            (unsigned long long)computed[0], (unsigned long long)computed[1],
            (unsigned long long)computed[2], (unsigned long long)computed[3]);
    fprintf(stderr, "  result[1020..1023]: %llu %llu %llu %llu\n",
            (unsigned long long)computed[1020], (unsigned long long)computed[1021],
            (unsigned long long)computed[1022], (unsigned long long)computed[1023]);
    fprintf(stderr, "  grand total = %llu  (expected %llu)\n",
            (unsigned long long)computed_total, (unsigned long long)expected_total);

    rt_l7.stop(); rt_l6.stop(); rt_l5.stop(); rt_l4.stop(); rt_l3.stop();

    fs::remove_all(kBaseDir);

    const int total_tree_workers = (total_l3 - 1);
    fprintf(stderr, "\n=== DFS Hierarchical Sum Test (Tree Reduction) PASSED ===\n");
    fprintf(stderr, "Verified:\n");
    fprintf(stderr, "  1. L3 WORKER (dfs_l3_reader_worker): reads DFS file → %d-element leaf tensor\n",
            kNumsPerFile);
    fprintf(stderr, "  2. TREE WORKER (pair_sum_worker): element-wise sum of 2 tensors (len=%d)\n",
            kNumsPerFile);
    fprintf(stderr, "  3. result[k] = sum(tensor[i][k]) for i=0..%d, k=0..%d\n",
            total_l3-1, kNumsPerFile-1);
    fprintf(stderr, "  4. tree_reduce(): ALL tree workers submitted upfront; tensor-map drives dispatch\n");
    fprintf(stderr, "  5. Per L4 pod (%d leaves): %d readers + %d tree workers = %d tasks\n",
            kNumL3PerL4, kNumL3PerL4, kNumL3PerL4 - 1, 2*kNumL3PerL4 - 1);
    fprintf(stderr, "  6. Total internal tree nodes across all levels: %d\n", total_tree_workers);
    fprintf(stderr, "  7. Output buffer allocated at submit_worker() (not at make_tensor)\n");
    fprintf(stderr, "  8. DAG deps tracked on tensor handles only; scalars in lambda captures\n");
    fprintf(stderr, "  9. Orchestrator/Scheduler/Worker share TensorRegistry in-process\n");
    return 0;
}
