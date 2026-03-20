#include "runtime/linqu_orchestrator_state.h"
#include "runtime/linqu_dispatcher.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace linqu;

// --- MockAsyncDispatcher (same pattern as test_completion_wiring) ---

class MockAsyncDispatcher : public LinquDispatcher {
public:
    bool dispatch(int32_t task_id, const LinquCoordinate&,
                  const std::string& kernel_so, LinquParam*, int) override {
        dispatched_ids_.push_back(task_id);
        dispatched_kernels_.push_back(kernel_so);
        return true;
    }

    void wait_all() override {}

    void simulate_complete(int32_t task_id) {
        DispatchResult r;
        r.task_id = task_id;
        r.status = TaskStatus::COMPLETED;
        r.error_code = 0;
        notify_completion(r);
    }

    std::vector<int32_t> dispatched_ids_;
    std::vector<std::string> dispatched_kernels_;
};

static void init_state(LinquOrchestratorState& state, MockAsyncDispatcher& disp) {
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 64;
    cfg.heap_ring_capacity = 65536;
    cfg.dep_pool_capacity = 512;
    cfg.tensormap_buckets = 64;
    cfg.tensormap_pool = 256;
    state.init(Level::HOST, LinquCoordinate{}, cfg);
    state.set_dispatcher(&disp);
}

static LinquCoordinate_C make_target(uint16_t l2_idx) {
    LinquCoordinate_C t;
    memset(&t, 0, sizeof(t));
    t.l2_idx = l2_idx;
    return t;
}

// --- Test 1: Basic group — 2 sub-tasks, only completes when both done ---

static void test_basic_group() {
    MockAsyncDispatcher disp;
    LinquOrchestratorState state;
    init_state(state, disp);
    LinquRuntime* rt = state.runtime();

    linqu_scope_begin(rt);

    LinquSubTaskSpec subs[2];
    subs[0] = {make_target(0), "kernel_a.so", nullptr, 0};
    subs[1] = {make_target(1), "kernel_b.so", nullptr, 0};

    linqu_submit_task_group(rt, "default.so", nullptr, 0, subs, 2);

    // Should have dispatched 2 sub-tasks with negative IDs
    assert(disp.dispatched_ids_.size() == 2);
    assert(disp.dispatched_ids_[0] < 0);
    assert(disp.dispatched_ids_[1] < 0);

    // Group task should be RUNNING (not COMPLETED yet)
    auto* desc = state.task_ring().get(0);
    assert(desc->is_group);
    assert(desc->group_size == 2);
    assert(desc->status == LinquTaskDescriptor::Status::RUNNING);

    // Complete first sub-task — group still RUNNING
    disp.simulate_complete(disp.dispatched_ids_[0]);
    assert(desc->status == LinquTaskDescriptor::Status::RUNNING);
    assert(desc->sub_complete_count.load() == 1);

    // Complete second sub-task — group now COMPLETED
    disp.simulate_complete(disp.dispatched_ids_[1]);
    assert(desc->status == LinquTaskDescriptor::Status::COMPLETED ||
           desc->status == LinquTaskDescriptor::Status::CONSUMED);

    linqu_scope_end(rt);
    printf("  test_basic_group: PASS\n");
}

// --- Test 2: Group with dependencies (DAG chain) ---
// task_A(OUTPUT=h1) → group(INPUT=h1, OUTPUT=h2) → task_B(INPUT=h2)

static void test_group_dependencies() {
    MockAsyncDispatcher disp;
    LinquOrchestratorState state;
    init_state(state, disp);
    LinquRuntime* rt = state.runtime();

    linqu_scope_begin(rt);

    LinquCoordinate_C t0 = make_target(0);

    // task_A: produces h1
    uint64_t h1 = linqu_alloc_tensor(rt, t0, 1024);
    LinquParam p_out_h1 = linqu_make_output(h1);
    linqu_submit_task(rt, t0, "producer.so", &p_out_h1, 1);
    assert(disp.dispatched_ids_.size() == 1);
    int32_t task_a_id = disp.dispatched_ids_[0];

    // group: consumes h1, produces h2
    uint64_t h2 = linqu_alloc_tensor(rt, t0, 1024);
    LinquParam group_params[2] = {linqu_make_input(h1), linqu_make_output(h2)};
    LinquSubTaskSpec subs[2];
    subs[0] = {make_target(0), "chip_0.so", nullptr, 0};
    subs[1] = {make_target(1), "chip_1.so", nullptr, 0};
    linqu_submit_task_group(rt, nullptr, group_params, 2, subs, 2);

    // Group should be PENDING (waiting for task_A)
    auto* group_desc = state.task_ring().get(1);
    assert(group_desc->is_group);
    assert(group_desc->status == LinquTaskDescriptor::Status::PENDING);
    // No sub-tasks dispatched yet
    assert(disp.dispatched_ids_.size() == 1);

    // task_B: consumes h2
    LinquParam p_in_h2 = linqu_make_input(h2);
    linqu_submit_task(rt, t0, "consumer.so", &p_in_h2, 1);
    // task_B should be dispatched immediately (fanin from group, but group not done)
    // Actually task_B depends on group's OUTPUT h2, so it has fanin=1 from group
    // It won't be dispatched until group completes

    // Complete task_A → group should start dispatching sub-tasks
    disp.simulate_complete(task_a_id);
    assert(group_desc->status == LinquTaskDescriptor::Status::RUNNING);
    // Sub-tasks should now be dispatched (2 new entries)
    assert(disp.dispatched_ids_.size() == 4); // task_a + task_b + 2 subs

    // Complete both sub-tasks → group COMPLETED → task_B should become ready
    // Find the sub-task IDs (negative)
    int32_t sub0 = disp.dispatched_ids_[2];
    int32_t sub1 = disp.dispatched_ids_[3];
    assert(sub0 < 0 && sub1 < 0);

    disp.simulate_complete(sub0);
    disp.simulate_complete(sub1);

    assert(group_desc->status == LinquTaskDescriptor::Status::COMPLETED ||
           group_desc->status == LinquTaskDescriptor::Status::CONSUMED);

    linqu_scope_end(rt);
    printf("  test_group_dependencies: PASS\n");
}

// --- Test 3: Scope interaction — group retires after scope_end ---

static void test_group_scope_retire() {
    MockAsyncDispatcher disp;
    LinquOrchestratorState state;
    init_state(state, disp);
    LinquRuntime* rt = state.runtime();

    linqu_scope_begin(rt);

    LinquSubTaskSpec subs[1];
    subs[0] = {make_target(0), "k.so", nullptr, 0};
    linqu_submit_task_group(rt, "k.so", nullptr, 0, subs, 1);

    auto* desc = state.task_ring().get(0);
    assert(desc->status == LinquTaskDescriptor::Status::RUNNING);

    // Complete the sub-task
    disp.simulate_complete(disp.dispatched_ids_[0]);
    assert(desc->status == LinquTaskDescriptor::Status::COMPLETED);

    // scope_end should allow retirement
    linqu_scope_end(rt);
    state.try_advance_ring_pointers();

    assert(desc->status == LinquTaskDescriptor::Status::CONSUMED);
    printf("  test_group_scope_retire: PASS\n");
}

// --- Test 4: SPMD — all sub-tasks use group kernel ---

static void test_group_spmd() {
    MockAsyncDispatcher disp;
    LinquOrchestratorState state;
    init_state(state, disp);
    LinquRuntime* rt = state.runtime();

    linqu_scope_begin(rt);

    LinquSubTaskSpec subs[3];
    subs[0] = {make_target(0), nullptr, nullptr, 0};  // kernel_so = NULL → use group kernel
    subs[1] = {make_target(1), nullptr, nullptr, 0};
    subs[2] = {make_target(2), nullptr, nullptr, 0};

    linqu_submit_task_group(rt, "spmd_kernel.so", nullptr, 0, subs, 3);

    assert(disp.dispatched_ids_.size() == 3);
    // All should use the group kernel
    for (int i = 0; i < 3; i++) {
        assert(disp.dispatched_kernels_[i] == "spmd_kernel.so");
    }

    // Complete all
    for (int i = 0; i < 3; i++)
        disp.simulate_complete(disp.dispatched_ids_[i]);

    auto* desc = state.task_ring().get(0);
    assert(desc->status == LinquTaskDescriptor::Status::COMPLETED ||
           desc->status == LinquTaskDescriptor::Status::CONSUMED);

    linqu_scope_end(rt);
    printf("  test_group_spmd: PASS\n");
}

// --- Test 5: Empty group — immediately completes ---

static void test_empty_group() {
    MockAsyncDispatcher disp;
    LinquOrchestratorState state;
    init_state(state, disp);
    LinquRuntime* rt = state.runtime();

    linqu_scope_begin(rt);

    linqu_submit_task_group(rt, "empty.so", nullptr, 0, nullptr, 0);

    auto* desc = state.task_ring().get(0);
    assert(desc->is_group);
    assert(desc->group_size == 0);
    assert(desc->status == LinquTaskDescriptor::Status::COMPLETED ||
           desc->status == LinquTaskDescriptor::Status::CONSUMED);

    // No sub-tasks dispatched
    assert(disp.dispatched_ids_.empty());

    linqu_scope_end(rt);
    printf("  test_empty_group: PASS\n");
}

// --- Test 6: Per-target private params ---

static void test_group_private_params() {
    MockAsyncDispatcher disp;
    LinquOrchestratorState state;
    init_state(state, disp);
    LinquRuntime* rt = state.runtime();

    linqu_scope_begin(rt);

    LinquParam priv0 = linqu_make_scalar(42);
    LinquParam priv1 = linqu_make_scalar(99);

    LinquSubTaskSpec subs[2];
    subs[0] = {make_target(0), "k.so", &priv0, 1};
    subs[1] = {make_target(1), "k.so", &priv1, 1};

    LinquParam group_p = linqu_make_scalar(7);
    linqu_submit_task_group(rt, "k.so", &group_p, 1, subs, 2);

    assert(disp.dispatched_ids_.size() == 2);

    // Complete both
    disp.simulate_complete(disp.dispatched_ids_[0]);
    disp.simulate_complete(disp.dispatched_ids_[1]);

    linqu_scope_end(rt);
    printf("  test_group_private_params: PASS\n");
}

// --- main ---

int main() {
    printf("test_task_group:\n");
    test_basic_group();
    test_group_dependencies();
    test_group_scope_retire();
    test_group_spmd();
    test_empty_group();
    test_group_private_params();
    printf("All group task tests passed.\n");
    return 0;
}
