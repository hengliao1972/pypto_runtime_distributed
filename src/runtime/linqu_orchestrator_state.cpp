#include "runtime/linqu_orchestrator_state.h"
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <chrono>

namespace linqu {

static LinquOrchestratorState*& hidden_state(LinquRuntime* rt) {
    static thread_local LinquOrchestratorState* s = nullptr;
    (void)rt;
    return s;
}

static void op_submit_task(LinquRuntime* rt, LinquCoordinate_C target,
                           const char* kernel_so, LinquParam* params, int num_params) {
    hidden_state(rt)->submit_task(target, kernel_so, params, num_params);
}
static void op_scope_begin(LinquRuntime* rt) { hidden_state(rt)->scope_begin(); }
static void op_scope_end(LinquRuntime* rt) { hidden_state(rt)->scope_end(); }
static uint64_t op_alloc_tensor(LinquRuntime* rt, LinquCoordinate_C t, size_t s) {
    return hidden_state(rt)->alloc_tensor(t, s);
}
static void op_free_tensor(LinquRuntime* rt, uint64_t h) {
    hidden_state(rt)->free_tensor(h);
}
static void op_done(LinquRuntime* rt) { hidden_state(rt)->orchestration_done(); }
static uint64_t op_reg_data(LinquRuntime*, LinquCoordinate_C, const void*, size_t s) {
    return s;
}

static LinquPeerList op_query_peers(LinquRuntime* rt, uint8_t level) {
    auto* state = hidden_state(rt);
    LinquPeerList pl;
    pl.peers = nullptr;
    pl.count = 0;

    if (!state) return pl;

    PeerRegistry* reg = state->peer_registry_ptr();
    FilesystemDiscovery* disc = state->fs_discovery_ptr();

    if (disc && reg) {
        disc->discover(*reg);
    }

    if (!reg) return pl;

    auto peers = reg->peers_at_level(level);
    if (peers.empty()) return pl;

    pl.count = static_cast<int>(peers.size());
    pl.peers = static_cast<LinquCoordinate_C*>(
        malloc(sizeof(LinquCoordinate_C) * peers.size()));
    for (size_t i = 0; i < peers.size(); i++) {
        LinquCoordinate_C c;
        c.l6_idx = peers[i].coord.l6_idx;
        c.l5_idx = peers[i].coord.l5_idx;
        c.l4_idx = peers[i].coord.l4_idx;
        c.l3_idx = peers[i].coord.l3_idx;
        c.l2_idx = peers[i].coord.l2_idx;
        c.l1_idx = peers[i].coord.l1_idx;
        c.l0_idx = peers[i].coord.l0_idx;
        pl.peers[i] = c;
    }
    return pl;
}

static LinquCoordinate_C op_self_coord(LinquRuntime* rt) {
    return hidden_state(rt)->self_coord();
}
static void op_wait_all(LinquRuntime* rt) { hidden_state(rt)->wait_all(); }
static void op_dump_snap(LinquRuntime* rt, const char* label) {
    hidden_state(rt)->take_snapshot(label ? label : "snapshot");
}
static void op_log_impl(LinquRuntime*, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
}
static void* op_get_pool(LinquRuntime*) { return nullptr; }

void LinquOrchestratorState::init(Level level, const LinquCoordinate& coord,
                                   const LinquOrchConfig_Internal& cfg) {
    level_ = level;
    coord_ = coord;
    task_ring_.init(cfg.task_ring_window);
    heap_ring_.init(cfg.heap_ring_capacity);
    dep_pool_.init(cfg.dep_pool_capacity);
    tensor_map_.init(cfg.tensormap_buckets, cfg.tensormap_pool);
    scope_stack_.init(MAX_SCOPE_DEPTH, cfg.task_ring_window * MAX_SCOPE_DEPTH);

    tasks_submitted_ = 0;
    next_tensor_handle_ = 1;
    done_ = false;

    memset(&ops_, 0, sizeof(ops_));
    ops_.submit_task = op_submit_task;
    ops_.scope_begin = op_scope_begin;
    ops_.scope_end = op_scope_end;
    ops_.alloc_tensor = op_alloc_tensor;
    ops_.free_tensor = op_free_tensor;
    ops_.orchestration_done = op_done;
    ops_.reg_data = op_reg_data;
    ops_.query_peers = op_query_peers;
    ops_.self_coord = op_self_coord;
    ops_.wait_all = op_wait_all;
    ops_.dump_ring_snapshot = op_dump_snap;
    ops_.log_error = op_log_impl;
    ops_.log_warn = op_log_impl;
    ops_.log_info = op_log_impl;
    ops_.log_debug = op_log_impl;
    ops_.get_tensor_pool = op_get_pool;

    rt_.ops = &ops_;
    hidden_state(&rt_) = this;
}

void LinquOrchestratorState::reset() {
    task_ring_.reset();
    heap_ring_.reset();
    dep_pool_.reset();
    tensor_map_.reset();
    scope_stack_.reset();
    tasks_submitted_ = 0;
    next_tensor_handle_ = 1;
    done_ = false;
    scope_begin_count_ = 0;
    scope_end_count_ = 0;
    free_tensor_count_ = 0;
    task_retire_count_ = 0;
    task_block_count_ = 0;
    heap_block_count_ = 0;
    peak_task_used_ = 0;
    peak_heap_used_ = 0;
    snapshots_.clear();
}

void LinquOrchestratorState::set_dispatcher(LinquDispatcher* d) {
    dispatcher_ = d;
    wire_dispatcher_callback();
}

void LinquOrchestratorState::wire_dispatcher_callback() {
    if (!dispatcher_) return;
    dispatcher_->set_completion_callback([this](const DispatchResult& r) {
        if (r.status == TaskStatus::COMPLETED) {
            on_task_complete(r.task_id);
        }
    });
}

void LinquOrchestratorState::submit_task(LinquCoordinate_C target,
                                          const char* kernel_so,
                                          LinquParam* params, int num_params) {
    if (!task_ring_.has_space()) {
        task_block_count_++;
        int spins = 0;
        while (!task_ring_.has_space()) {
            try_advance_ring_pointers();
            if (task_ring_.has_space()) break;
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            if (++spins > 100000) {
                fprintf(stderr, "[LinquOrchestrator] FATAL: task ring full, "
                        "no slots freed after 10s. Deadlock?\n");
                assert(false && "task ring deadlock");
            }
        }
    }

    int32_t tid = task_ring_.alloc();
    auto* desc = task_ring_.get(tid);
    desc->kernel_so = kernel_so ? kernel_so : "";
    desc->key.task_id = static_cast<uint32_t>(tid);
    desc->key.scope_depth = static_cast<uint16_t>(scope_stack_.depth() >= 0 ? scope_stack_.depth() : 0);

    uint32_t fanin = 0;
    for (int i = 0; i < num_params; i++) {
        if (params[i].type == LINQU_PARAM_INPUT || params[i].type == LINQU_PARAM_INOUT) {
            auto r = tensor_map_.lookup(params[i].handle);
            if (r.found) {
                desc->dep_list_head = dep_pool_.prepend(desc->dep_list_head, r.producer_task_id);
                auto* producer = task_ring_.get(r.producer_task_id);
                producer->fanout_count++;
                producer->fanout_list_head = dep_pool_.prepend(producer->fanout_list_head, tid);
                fanin++;
            }
        }
        if (params[i].type == LINQU_PARAM_OUTPUT || params[i].type == LINQU_PARAM_INOUT) {
            tensor_map_.insert(params[i].handle, tid);
        }
    }
    desc->fanin_count = fanin;

    if (scope_stack_.depth() >= 0) {
        scope_stack_.add_task(tid);
    }

    desc->heap_end = heap_ring_.top();

    tasks_submitted_++;

    auto tu = static_cast<size_t>(task_ring_.active_count());
    if (tu > peak_task_used_) peak_task_used_ = tu;
    auto hu = heap_ring_.used();
    if (hu > peak_heap_used_) peak_heap_used_ = hu;

    LinquCoordinate tgt;
    tgt.l6_idx = target.l6_idx;
    tgt.l5_idx = target.l5_idx;
    tgt.l4_idx = target.l4_idx;
    tgt.l3_idx = target.l3_idx;
    tgt.l2_idx = target.l2_idx;
    tgt.l1_idx = target.l1_idx;
    tgt.l0_idx = target.l0_idx;

    desc->status = LinquTaskDescriptor::Status::RUNNING;

    if (dispatcher_) {
        dispatcher_->dispatch(tid, tgt, kernel_so ? kernel_so : "", params, num_params);
    } else if (dispatch_fn_) {
        dispatch_fn_(tid, tgt, kernel_so ? kernel_so : "", params, num_params);
        desc->status = LinquTaskDescriptor::Status::COMPLETED;
        propagate_completion(tid);
    } else {
        desc->status = LinquTaskDescriptor::Status::COMPLETED;
        propagate_completion(tid);
    }
}

void LinquOrchestratorState::on_task_complete(int32_t task_id) {
    std::lock_guard<std::mutex> lk(scheduler_mu_);
    auto* desc = task_ring_.get(task_id);
    if (desc->status == LinquTaskDescriptor::Status::RUNNING) {
        desc->status = LinquTaskDescriptor::Status::COMPLETED;
        propagate_completion(task_id);
    }
}

void LinquOrchestratorState::propagate_completion(int32_t task_id) {
    auto* desc = task_ring_.get(task_id);

    // 1) Walk completed task's fanin list (producers). This task completing
    //    means it no longer needs the producers' outputs -> increment
    //    each producer's fanout_refcount toward CONSUMED.
    int32_t fanin_cur = desc->dep_list_head;
    while (fanin_cur > 0) {
        auto* entry = dep_pool_.get(fanin_cur);
        if (entry) {
            auto* producer = task_ring_.get(entry->task_id);
            producer->fanout_refcount++;
            check_task_consumed(entry->task_id);
        }
        fanin_cur = entry ? entry->next : -1;
    }

    // 2) Walk completed task's fanout list (consumers). Producer completing
    //    means consumers can now read its output -> increment each
    //    consumer's fanin_refcount toward READY.
    int32_t fanout_cur = desc->fanout_list_head;
    while (fanout_cur > 0) {
        auto* entry = dep_pool_.get(fanout_cur);
        if (entry) {
            auto* consumer = task_ring_.get(entry->task_id);
            consumer->fanin_refcount++;
        }
        fanout_cur = entry ? entry->next : -1;
    }

    check_task_consumed(task_id);
}

void LinquOrchestratorState::check_consumers_ready(int32_t producer_task_id) {
    auto* desc = task_ring_.get(producer_task_id);
    int32_t cur = desc->fanout_list_head;
    while (cur > 0) {
        auto* entry = dep_pool_.get(cur);
        if (entry) {
            auto* consumer = task_ring_.get(entry->task_id);
            if (consumer->fanin_refcount >= consumer->fanin_count &&
                consumer->status == LinquTaskDescriptor::Status::PENDING) {
                consumer->status = LinquTaskDescriptor::Status::READY;
            }
        }
        cur = entry ? entry->next : -1;
    }
}

void LinquOrchestratorState::on_scope_release(int32_t task_id) {
    std::lock_guard<std::mutex> lk(scheduler_mu_);
    check_task_consumed(task_id);
}

void LinquOrchestratorState::check_task_consumed(int32_t task_id) {
    auto* desc = task_ring_.get(task_id);
    if (desc->status == LinquTaskDescriptor::Status::COMPLETED &&
        desc->fanout_refcount >= desc->fanout_count) {
        desc->status = LinquTaskDescriptor::Status::CONSUMED;
    }
}

void LinquOrchestratorState::try_advance_ring_pointers() {
    std::lock_guard<std::mutex> lk(scheduler_mu_);
    int32_t cursor = task_ring_.last_task_alive();
    int32_t current = task_ring_.current_index();

    while (cursor < current) {
        auto* desc = task_ring_.get(cursor);
        if (desc->status != LinquTaskDescriptor::Status::CONSUMED) break;
        cursor++;
        task_retire_count_++;
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

void LinquOrchestratorState::scope_begin() {
    scope_stack_.scope_begin();
    scope_begin_count_++;
}

void LinquOrchestratorState::scope_end() {
    scope_stack_.scope_end(scope_get_desc, this);
    scope_end_count_++;

    {
        std::lock_guard<std::mutex> lk(scheduler_mu_);
        int32_t cursor = task_ring_.last_task_alive();
        int32_t current = task_ring_.current_index();
        for (int32_t i = cursor; i < current; i++) {
            check_task_consumed(i);
        }
    }

    try_advance_ring_pointers();
}

LinquTaskDescriptor* LinquOrchestratorState::scope_get_desc(int32_t tid, void* ctx) {
    return static_cast<LinquOrchestratorState*>(ctx)->task_ring_.get(tid);
}

uint64_t LinquOrchestratorState::alloc_tensor(LinquCoordinate_C, size_t size_bytes) {
    uint64_t handle = next_tensor_handle_++;
    if (size_bytes > 0) {
        void* p = nullptr;
        if (!heap_ring_.try_alloc(size_bytes, &p)) {
            heap_block_count_++;
            int spins = 0;
            while (!heap_ring_.try_alloc(size_bytes, &p)) {
                try_advance_ring_pointers();
                if (heap_ring_.try_alloc(size_bytes, &p)) break;
                std::this_thread::sleep_for(std::chrono::microseconds(100));
                if (++spins > 100000) {
                    fprintf(stderr, "[LinquOrchestrator] FATAL: heap ring full, "
                            "no space freed after 10s. Deadlock?\n");
                    assert(false && "heap ring deadlock");
                }
            }
        }
        auto hu = heap_ring_.used();
        if (hu > peak_heap_used_) peak_heap_used_ = hu;
    }
    return handle;
}

void LinquOrchestratorState::free_tensor(uint64_t handle) {
    (void)handle;
    free_tensor_count_++;
}

void LinquOrchestratorState::orchestration_done() {
    done_ = true;
}

void LinquOrchestratorState::wait_all() {
    if (dispatcher_) {
        dispatcher_->wait_all();
    }
}

RingMetrics LinquOrchestratorState::current_metrics() const {
    RingMetrics m;
    m.level = static_cast<uint8_t>(level_value(level_));
    m.depth = 0;
    m.task_ring_capacity = static_cast<size_t>(task_ring_.window_size());
    m.task_ring_peak_used = peak_task_used_;
    m.task_alloc_count = static_cast<size_t>(tasks_submitted_);
    m.task_retire_count = task_retire_count_;
    m.task_block_count = task_block_count_;
    m.buffer_ring_capacity = heap_ring_.capacity();
    m.buffer_ring_peak_used = peak_heap_used_;
    m.scope_begin_count = scope_begin_count_;
    m.scope_end_count = scope_end_count_;
    m.free_tensor_count = free_tensor_count_;
    return m;
}

ProfileReport LinquOrchestratorState::generate_profile(const std::string& node_id) const {
    ProfileReport rpt;
    rpt.node_id = node_id;
    rpt.level = static_cast<uint8_t>(level_value(level_));
    rpt.metrics.push_back(current_metrics());
    rpt.snapshots = snapshots_;
    return rpt;
}

void LinquOrchestratorState::take_snapshot(const std::string& label) {
    RingSnapshot snap;
    snap.level = static_cast<uint8_t>(level_value(level_));
    snap.depth = static_cast<uint16_t>(scope_stack_.depth() >= 0 ? scope_stack_.depth() : 0);
    snap.label = label;
    snap.timestamp_us = now_us();
    snap.task_capacity = static_cast<size_t>(task_ring_.window_size());
    snap.task_used = static_cast<size_t>(task_ring_.active_count());
    snap.task_head = static_cast<size_t>(task_ring_.last_task_alive());
    snap.task_tail = static_cast<size_t>(task_ring_.current_index());
    snap.buffer_capacity = heap_ring_.capacity();
    snap.buffer_used = heap_ring_.used();
    snap.buffer_head = heap_ring_.tail();
    snap.buffer_tail = heap_ring_.top();
    snap.alloc_count = static_cast<size_t>(tasks_submitted_);
    snap.retire_count = task_retire_count_;
    snap.free_count = free_tensor_count_;
    snapshots_.push_back(snap);
}

LinquCoordinate_C LinquOrchestratorState::self_coord() const {
    LinquCoordinate_C c;
    c.l6_idx = coord_.l6_idx;
    c.l5_idx = coord_.l5_idx;
    c.l4_idx = coord_.l4_idx;
    c.l3_idx = coord_.l3_idx;
    c.l2_idx = coord_.l2_idx;
    c.l1_idx = coord_.l1_idx;
    c.l0_idx = coord_.l0_idx;
    return c;
}

}
