#include "runtime/level_runtime.h"
#include <cassert>
#include <chrono>
#include <cstdio>

namespace linqu {

// =========================================================================
// Construction / Destruction
// =========================================================================

LevelRuntime::LevelRuntime(int level, int sched_threads, int worker_threads,
                           const LevelRuntimeConfig& cfg)
    : level_(level),
      sched_count_(sched_threads  > 0 ? sched_threads  : 1),
      worker_count_(worker_threads > 0 ? worker_threads : 1)
{
    task_ring_.init(cfg.task_ring_window);
    heap_ring_.init(cfg.heap_ring_capacity);
    dep_pool_.init(cfg.dep_pool_capacity);
    tensor_map_.init(cfg.tensormap_buckets, cfg.tensormap_pool);
    scope_stack_.init(8, cfg.task_ring_window * 8);

    rq_capacity_ = cfg.ready_queue_capacity;
    rq_buf_.resize(static_cast<size_t>(rq_capacity_), -1);
    rq_head_.store(0, std::memory_order_relaxed);
    rq_tail_.store(0, std::memory_order_relaxed);

    const auto ws = static_cast<size_t>(cfg.task_ring_window);
    task_promises_.resize(ws);
    task_outputs_.resize(ws);
    task_external_inputs_.resize(ws);
    task_external_ready_.resize(ws);
    task_trace_pids_.resize(ws, -1);
}

LevelRuntime::~LevelRuntime() { stop(); }

// =========================================================================
// Start / Stop
// =========================================================================

void LevelRuntime::set_trace_writer(TraceWriter* tw) {
    trace_ = tw;
    if (!tw || !tw->enabled()) return;

    // Register level-wide process for shared scheduler events.
    std::string pname = "L" + std::to_string(level_);
    tw->set_process_name(level_, pname);
    for (int i = 0; i < sched_count_; i++)
        tw->set_thread_name(level_, trace_tid_sched(i),
                            "Scheduler-" + std::to_string(i));
}

void LevelRuntime::register_trace_instance(int32_t trace_pid,
                                            const std::string& label) {
    if (!trace_ || !trace_->enabled()) return;
    trace_->set_process_name(trace_pid, label);
    trace_->set_thread_name(trace_pid, trace_tid_orch(), "Orchestrator");
    for (int i = 0; i < worker_count_; i++)
        trace_->set_thread_name(trace_pid, trace_tid_worker(i),
                                "Worker-" + std::to_string(i));
}

void LevelRuntime::start() {
    std::lock_guard<std::mutex> lk(state_mu_);
    if (running_) return;
    running_        = true;
    stop_requested_ = false;

    orch_thread_ = std::thread([this] { orchestrator_loop(); });
    for (int i = 0; i < sched_count_;  i++)
        sched_threads_.emplace_back([this, i] { scheduler_loop(i); });
    for (int i = 0; i < worker_count_; i++)
        worker_threads_.emplace_back([this, i] { worker_loop(i); });
}

void LevelRuntime::stop() {
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        if (!running_) return;
        stop_requested_ = true;
    }
    orch_cv_.notify_all();
    sched_cv_.notify_all();
    worker_cv_.notify_all();

    if (orch_thread_.joinable()) orch_thread_.join();
    for (auto& t : sched_threads_)  if (t.joinable()) t.join();
    for (auto& t : worker_threads_) if (t.joinable()) t.join();
    sched_threads_.clear();
    worker_threads_.clear();

    std::lock_guard<std::mutex> lk(state_mu_);
    running_ = false;
}

bool LevelRuntime::should_stop() const {
    std::lock_guard<std::mutex> lk(state_mu_);
    return stop_requested_;
}

// =========================================================================
// make_tensor — metadata only, no HeapRing allocation
// =========================================================================

LinquTensor LevelRuntime::make_tensor(size_t count) {
    LinquTensor t;
    t.handle   = alloc_tensor_handle();
    t.count    = count;
    t.data_ref = std::make_shared<uint64_t*>(nullptr);
    t.ready    = std::make_shared<std::atomic<bool>>(false);
    return t;
}

// =========================================================================
// Per-task spinlock (aligns with simpler pto2_fanout_lock/unlock)
// =========================================================================

void LevelRuntime::fanout_lock(LinquTaskDescriptor* desc) {
    while (__atomic_exchange_n(&desc->fanout_lock, 1, __ATOMIC_ACQUIRE) != 0) {
        // spin
    }
}

void LevelRuntime::fanout_unlock(LinquTaskDescriptor* desc) {
    __atomic_store_n(&desc->fanout_lock, 0, __ATOMIC_RELEASE);
}

// =========================================================================
// add_consumer_to_producer — aligns with simpler pto2_add_consumer_to_producer
//
// Under the producer's spinlock:
//   1. Prepend consumer_id to producer's fanout chain
//   2. Increment producer's fanout_count
//   3. If producer already COMPLETED, directly bump consumer's fanin_refcount
// =========================================================================

void LevelRuntime::add_consumer_to_producer(int32_t producer_id,
                                             int32_t consumer_id) {
    auto* producer = task_ring_.get(producer_id);
    fanout_lock(producer);

    producer->fanout_list_head =
        dep_pool_.prepend(producer->fanout_list_head, consumer_id);
    producer->fanout_count++;

    if (producer->status == LinquTaskDescriptor::Status::COMPLETED ||
        producer->status == LinquTaskDescriptor::Status::CONSUMED) {
        auto* consumer = task_ring_.get(consumer_id);
        consumer->atomic_fanin_refcount.fetch_add(1, std::memory_order_seq_cst);
    }

    fanout_unlock(producer);
}

// =========================================================================
// Ready queue — circular buffer of task_ids (simpler PTO2ReadyQueue style)
// =========================================================================

void LevelRuntime::enqueue_ready(int32_t task_id) {
    {
        std::lock_guard<std::mutex> lk(worker_mu_);
        int32_t tail = rq_tail_.load(std::memory_order_relaxed);
        rq_buf_[tail % rq_capacity_] = task_id;
        rq_tail_.store(tail + 1, std::memory_order_release);
    }
    worker_cv_.notify_one();
}

int32_t LevelRuntime::dequeue_ready() {
    int32_t head = rq_head_.load(std::memory_order_relaxed);
    int32_t tail = rq_tail_.load(std::memory_order_acquire);
    if (head >= tail) return -1;
    int32_t tid = rq_buf_[head % rq_capacity_];
    rq_head_.store(head + 1, std::memory_order_release);
    return tid;
}

// =========================================================================
// Atomic CAS helper: PENDING → READY transition.
//
// Multiple threads may concurrently decide a task's fanin is satisfied
// (propagate_completion, submit_worker, scheduler_loop).  The CAS ensures
// exactly ONE thread wins the transition and enqueues the task, preventing
// duplicate ready-queue entries that would cause "Promise already satisfied".
// This matches simpler's single-writer ready transition guarantee.
// =========================================================================

static bool try_set_ready(LinquTaskDescriptor* desc) {
    uint8_t expected = static_cast<uint8_t>(LinquTaskDescriptor::Status::PENDING);
    uint8_t desired  = static_cast<uint8_t>(LinquTaskDescriptor::Status::READY);
    return __atomic_compare_exchange_n(
        reinterpret_cast<volatile uint8_t*>(&desc->status),
        &expected, desired,
        /*weak=*/false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

// =========================================================================
// submit_worker — aligns with simpler pto2_submit_task
// =========================================================================

std::future<void> LevelRuntime::submit_worker(
    const std::string&       name,
    std::function<void()>    fn,
    std::vector<LinquTensor> inputs,
    std::vector<LinquTensor> outputs,
    int32_t trace_pid)
{
    // Step 0: Back-pressure if TaskRing full (simpler spin-wait pattern).
    if (!task_ring_.has_space()) {
        int spins = 0;
        while (!task_ring_.has_space()) {
            try_advance_ring_pointers();
            if (task_ring_.has_space()) break;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            if (++spins > 100000) {
                fprintf(stderr, "[LevelRuntime L%d] FATAL: task ring full, "
                        "no slots freed after 10s. Deadlock?\n", level_);
                assert(false && "task ring deadlock");
            }
        }
    }

    // Step 1: Alloc task slot from TaskRing.
    int32_t tid = task_ring_.alloc();
    auto* desc = task_ring_.get(tid);
    desc->key.task_id = static_cast<uint32_t>(tid);
    desc->kernel_so = name;  // task name, same field simpler uses
    desc->fn = std::move(fn);

    // Per-slot auxiliary data (indexed by task_id % window_size).
    int32_t slot = tid & (task_ring_.window_size() - 1);
    auto promise = std::make_shared<std::promise<void>>();
    task_promises_[slot] = promise;
    task_outputs_[slot] = outputs;
    task_external_inputs_[slot].clear();
    task_external_ready_[slot].clear();
    task_trace_pids_[slot] = trace_pid;

    // Step 2: Scan inputs — build fanin dep chain AND collect producer IDs.
    // We do NOT add to producers' fanout lists yet; that happens in step 7
    // AFTER fanin_count and status are set, to avoid a race where
    // propagate_completion sees the consumer before it is fully initialized.
    uint32_t fanin = 0;
    bool has_external = false;
    std::vector<int32_t> within_level_producers;

    for (const auto& in : inputs) {
        if (!in.is_valid()) continue;
        auto r = tensor_map_.lookup(in.handle);
        if (r.found) {
            desc->dep_list_head =
                dep_pool_.prepend(desc->dep_list_head, r.producer_task_id);
            within_level_producers.push_back(r.producer_task_id);
            fanin++;
        } else {
            if (in.is_ready()) {
                // Already ready — no dependency needed.
            } else {
                task_external_inputs_[slot].push_back(in.handle);
                task_external_ready_[slot].push_back(in.ready);
                has_external = true;
                fanin++;
            }
        }
    }

    // Step 3: Allocate output buffers from HeapRing (simpler packed buffer).
    size_t total_output_bytes = 0;
    for (auto& out : outputs) {
        total_output_bytes += out.count * sizeof(uint64_t);
    }
    if (total_output_bytes > 0) {
        void* buf = heap_ring_.alloc(total_output_bytes);
        auto* ptr = static_cast<uint64_t*>(buf);
        for (auto& out : outputs) {
            *out.data_ref = ptr;
            for (size_t k = 0; k < out.count; k++) ptr[k] = 0;
            ptr += out.count;
        }
        task_outputs_[slot] = outputs;
    }

    // Step 4: Register outputs in TensorMap.
    for (const auto& out : outputs) {
        tensor_map_.insert(out.handle, tid);
    }

    // Step 5: Register in scope (fanout_count starts at 1 for scope ref).
    if (scope_stack_.depth() >= 0) {
        scope_stack_.add_task(tid);
    }

    // Step 6: Record heap high-water mark for ring retirement.
    desc->heap_end = heap_ring_.top();

    // Step 7: Finalize fanin_count and status BEFORE exposing to fanout.
    // This ensures propagate_completion sees correct fanin_count if it
    // races with the add_consumer_to_producer calls below.
    desc->fanin_count = fanin;

    if (fanin == 0) {
        desc->status = LinquTaskDescriptor::Status::READY;
        enqueue_ready(tid);
    } else {
        desc->status = LinquTaskDescriptor::Status::PENDING;
        std::atomic_thread_fence(std::memory_order_seq_cst);

        // Now add to each producer's fanout list, making this task visible
        // to propagate_completion running on worker threads.
        for (int32_t prod_id : within_level_producers) {
            add_consumer_to_producer(prod_id, tid);
        }

        // Check if fanin already fully satisfied (producers completed
        // during the add_consumer loop above).  CAS prevents double-enqueue
        // if propagate_completion already transitioned us to READY.
        uint32_t already =
            desc->atomic_fanin_refcount.load(std::memory_order_seq_cst);
        if (already >= fanin) {
            if (try_set_ready(desc)) {
                enqueue_ready(tid);
            }
        } else if (has_external) {
            sched_cv_.notify_all();
        }
    }

    return promise->get_future();
}

// =========================================================================
// on_task_complete / propagate_completion — aligns with simpler
// pto2_scheduler_on_task_complete
// =========================================================================

void LevelRuntime::on_task_complete(int32_t task_id) {
    auto* desc = task_ring_.get(task_id);
    desc->status = LinquTaskDescriptor::Status::COMPLETED;

    propagate_completion(task_id);
}

void LevelRuntime::propagate_completion(int32_t task_id) {
    auto* desc = task_ring_.get(task_id);

    // Step 1: Walk fanout chain (consumers) under the fanout lock.
    // The lock serializes with add_consumer_to_producer, preventing the
    // race where both this function and add_consumer_to_producer bump the
    // same consumer's refcount for the same dependency.
    // CAS try_set_ready additionally guards against double-enqueue with
    // submit_worker step 7 and scheduler_loop.
    {
        fanout_lock(desc);
        int32_t cur = desc->fanout_list_head;
        while (cur > 0) {
            auto* entry = dep_pool_.get(cur);
            if (entry) {
                auto* consumer = task_ring_.get(entry->task_id);
                uint32_t prev = consumer->atomic_fanin_refcount.fetch_add(
                    1, std::memory_order_seq_cst);
                if (prev + 1 >= consumer->fanin_count) {
                    if (try_set_ready(consumer)) {
                        enqueue_ready(entry->task_id);
                    }
                }
                cur = entry->next;
            } else {
                break;
            }
        }
        fanout_unlock(desc);
    }

    // Step 2: Walk fanin chain (producers). This task completing means it
    // no longer needs the producers' outputs → increment each producer's
    // fanout_refcount toward CONSUMED.
    // (Aligns with simpler: pto2_scheduler_release_producer)
    {
        int32_t cur = desc->dep_list_head;
        while (cur > 0) {
            auto* entry = dep_pool_.get(cur);
            if (entry) {
                auto* producer = task_ring_.get(entry->task_id);
                producer->atomic_fanout_refcount.fetch_add(1, std::memory_order_seq_cst);
                check_task_consumed(entry->task_id);
                cur = entry->next;
            } else {
                break;
            }
        }
    }

    // Step 3: Check self for CONSUMED.
    check_task_consumed(task_id);
}

// =========================================================================
// check_task_consumed — aligns with simpler check_and_handle_consumed
// =========================================================================

void LevelRuntime::check_task_consumed(int32_t task_id) {
    auto* desc = task_ring_.get(task_id);
    if (desc->status != LinquTaskDescriptor::Status::COMPLETED) return;

    uint32_t fc = desc->fanout_count;
    uint32_t rc = desc->atomic_fanout_refcount.load(std::memory_order_seq_cst);
    if (rc >= fc) {
        desc->status = LinquTaskDescriptor::Status::CONSUMED;
        if (task_id == task_ring_.last_task_alive()) {
            try_advance_ring_pointers();
        }
    }
}

// =========================================================================
// try_advance_ring_pointers — aligns with simpler
// pto2_scheduler_advance_ring_pointers
// =========================================================================

void LevelRuntime::try_advance_ring_pointers() {
    int32_t cursor  = task_ring_.last_task_alive();
    int32_t current = task_ring_.current_index();

    while (cursor < current) {
        auto* desc = task_ring_.get(cursor);
        if (desc->status != LinquTaskDescriptor::Status::CONSUMED) break;
        cursor++;
    }

    if (cursor > task_ring_.last_task_alive()) {
        task_ring_.set_last_task_alive(cursor);
        tensor_map_.invalidate_below(cursor);

        if (cursor > 0) {
            auto* last_retired = task_ring_.get(cursor - 1);
            if (last_retired->heap_end > heap_ring_.tail()) {
                heap_ring_.retire_to(last_retired->heap_end);
            }
        }
    }
}

// =========================================================================
// Thread loops
// =========================================================================

void LevelRuntime::orchestrator_loop() {
    while (true) {
        OrchJob job;
        {
            std::unique_lock<std::mutex> lk(orch_mu_);
            orch_cv_.wait(lk, [this] {
                return should_stop() || !orch_queue_.empty();
            });
            if (should_stop() && orch_queue_.empty()) break;
            job = std::move(orch_queue_.front());
            orch_queue_.pop();
        }
        if (trace_ && trace_->enabled()) {
            int32_t tpid = job.trace_pid >= 0 ? job.trace_pid : level_;
            int64_t t0 = trace_->now_us();
            job.fn();
            int64_t dur = trace_->now_us() - t0;
            trace_->add_complete(job.name, "orchestrator",
                                 tpid, trace_tid_orch(), t0, dur);
        } else {
            job.fn();
        }
    }
}

// Scheduler loop — handles ONLY cross-level external tensor readiness.
// Within-level deps are resolved event-driven via propagate_completion.
// This avoids the 2ms polling overhead for pure within-level workloads.
void LevelRuntime::scheduler_loop(int sched_idx) {
    int scan_seq = 0;
    while (true) {
        {
            std::unique_lock<std::mutex> lk(sched_mu_);
            sched_cv_.wait_for(lk, std::chrono::milliseconds(2),
                               [this] { return should_stop(); });
        }
        if (should_stop()) break;

        int64_t t0 = 0;
        bool tracing = trace_ && trace_->enabled();
        if (tracing) t0 = trace_->now_us();

        int promoted = 0;

        // Scan tasks with external (cross-level) inputs.
        int32_t last = task_ring_.last_task_alive();
        int32_t cur  = task_ring_.current_index();
        for (int32_t i = last; i < cur; i++) {
            auto* desc = task_ring_.get(i);
            if (desc->status != LinquTaskDescriptor::Status::PENDING)
                continue;

            int32_t slot = i & (task_ring_.window_size() - 1);
            auto& ext_ready = task_external_ready_[slot];
            if (ext_ready.empty()) continue;

            bool all_ext_ready = true;
            for (auto& rp : ext_ready) {
                if (!rp->load(std::memory_order_acquire)) {
                    all_ext_ready = false;
                    break;
                }
            }
            if (!all_ext_ready) continue;

            uint32_t ext_count = static_cast<uint32_t>(ext_ready.size());
            uint32_t prev = desc->atomic_fanin_refcount.fetch_add(
                ext_count, std::memory_order_seq_cst);
            ext_ready.clear();

            if (prev + ext_count >= desc->fanin_count) {
                if (try_set_ready(desc)) {
                    enqueue_ready(i);
                    promoted++;
                }
            }
        }

        if (tracing && promoted > 0) {
            int64_t dur = trace_->now_us() - t0;
            std::string label = "sched_scan_" + std::to_string(scan_seq++)
                                + " +" + std::to_string(promoted);
            trace_->add_complete(label.c_str(), "scheduler",
                                 level_, trace_tid_sched(sched_idx), t0, dur);
        }
    }
}

void LevelRuntime::worker_loop(int worker_idx) {
    while (true) {
        int32_t tid;
        {
            std::unique_lock<std::mutex> lk(worker_mu_);
            worker_cv_.wait(lk, [this] {
                return should_stop() ||
                       rq_head_.load(std::memory_order_acquire) <
                       rq_tail_.load(std::memory_order_acquire);
            });
            if (should_stop() &&
                rq_head_.load(std::memory_order_acquire) >=
                rq_tail_.load(std::memory_order_acquire))
                break;
            tid = dequeue_ready();
            if (tid < 0) continue;
        }

        auto* desc = task_ring_.get(tid);
        desc->status = LinquTaskDescriptor::Status::RUNNING;
        int32_t slot = tid & (task_ring_.window_size() - 1);

        int64_t t0 = 0;
        bool tracing = trace_ && trace_->enabled();
        if (tracing) t0 = trace_->now_us();

        desc->fn();

        if (tracing) {
            int32_t tpid = task_trace_pids_[slot];
            if (tpid < 0) tpid = level_;
            int64_t dur = trace_->now_us() - t0;
            trace_->add_complete(desc->kernel_so, "worker",
                                 tpid, trace_tid_worker(worker_idx),
                                 t0, dur);
        }
        for (auto& out : task_outputs_[slot]) {
            out.mark_ready();
        }

        if (task_promises_[slot]) {
            task_promises_[slot]->set_value();
            task_promises_[slot].reset();
        }

        on_task_complete(tid);
        sched_cv_.notify_all();
    }
}

}
