#include "runtime/linqu_orchestrator_state.h"
#include "runtime/linqu_dispatcher.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace linqu;
using Status = LinquTaskDescriptor::Status;

class MockAsyncDispatcher : public LinquDispatcher {
public:
    bool dispatch(int32_t task_id, const LinquCoordinate&,
                  const std::string&, LinquParam*, int) override {
        dispatched_.push_back(task_id);
        return true;
    }
    void wait_all() override {}

    void complete(int32_t task_id) {
        DispatchResult r;
        r.task_id = task_id;
        r.status = TaskStatus::COMPLETED;
        r.error_code = 0;
        notify_completion(r);
    }

    std::vector<int32_t> dispatched_;
};

static void test_fanin_fanout_counts() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 32;
    cfg.heap_ring_capacity = 65536;
    cfg.dep_pool_capacity = 256;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target{};

    uint64_t h_a = linqu_alloc_tensor(rt, target, 1024);
    uint64_t h_b = linqu_alloc_tensor(rt, target, 1024);

    linqu_scope_begin(rt);

    // t0: produce c = f(a, b)
    uint64_t h_c = linqu_alloc_tensor(rt, target, 1024);
    LinquParam p0[] = {linqu_make_input(h_a), linqu_make_input(h_b), linqu_make_output(h_c)};
    linqu_submit_task(rt, target, "produce.so", p0, 3);

    // t1: consume c -> d
    uint64_t h_d = linqu_alloc_tensor(rt, target, 1024);
    LinquParam p1[] = {linqu_make_input(h_c), linqu_make_output(h_d)};
    linqu_submit_task(rt, target, "consume1.so", p1, 2);

    // t2: consume c -> e (second consumer of c)
    uint64_t h_e = linqu_alloc_tensor(rt, target, 1024);
    LinquParam p2[] = {linqu_make_input(h_c), linqu_make_output(h_e)};
    linqu_submit_task(rt, target, "consume2.so", p2, 2);

    auto* d0 = state.task_ring().get(0);
    auto* d1 = state.task_ring().get(1);
    auto* d2 = state.task_ring().get(2);

    // t0: fanout_count = 1 (scope) + 2 (consumers t1, t2) = 3
    assert(d0->fanout_count == 3);
    // t1: fanin_count = 1 (depends on t0)
    assert(d1->fanin_count == 1);
    // t2: fanin_count = 1 (depends on t0)
    assert(d2->fanin_count == 1);
    // t0: fanin_count = 0 (no deps since h_a, h_b are pre-allocated with no producer)
    assert(d0->fanin_count == 0);

    linqu_scope_end(rt);

    // After scope_end, each task gets fanout_refcount++ from scope
    assert(d0->fanout_refcount >= 1);
    assert(d1->fanout_refcount >= 1);
    assert(d2->fanout_refcount >= 1);

    printf("  fanin_fanout_counts: PASS\n");
}

static void test_state_machine_async() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 16;
    cfg.heap_ring_capacity = 65536;
    cfg.dep_pool_capacity = 128;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    MockAsyncDispatcher disp;
    state.set_dispatcher(&disp);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target{};

    uint64_t h_a = linqu_alloc_tensor(rt, target, 256);

    linqu_scope_begin(rt);

    // t0: produce b from a
    uint64_t h_b = linqu_alloc_tensor(rt, target, 256);
    LinquParam p0[] = {linqu_make_input(h_a), linqu_make_output(h_b)};
    linqu_submit_task(rt, target, "t0.so", p0, 2);

    // t1: consume b
    uint64_t h_c = linqu_alloc_tensor(rt, target, 256);
    LinquParam p1[] = {linqu_make_input(h_b), linqu_make_output(h_c)};
    linqu_submit_task(rt, target, "t1.so", p1, 2);

    auto* d0 = state.task_ring().get(0);
    auto* d1 = state.task_ring().get(1);

    assert(d0->status == Status::RUNNING);
    assert(d1->status == Status::RUNNING);

    // Complete t1 first (consumer) -> should increment t0's fanout_refcount
    disp.complete(1);
    assert(d1->status == Status::COMPLETED);

    // t0: fanout_count=2 (scope+t1), fanout_refcount should be 1 now
    // (t1 completed -> t0 fanout_refcount++)
    assert(d0->fanout_refcount == 1);

    // Complete t0 -> should increment t1's fanin_refcount
    disp.complete(0);
    assert(d0->status == Status::COMPLETED);
    assert(d1->fanin_refcount >= 1);

    linqu_scope_end(rt);

    // After scope_end: each task gets +1 fanout_refcount from scope
    // t0: fanout_count=2, fanout_refcount should be >= 2 (1 from t1 complete + 1 from scope)
    assert(d0->fanout_refcount >= 2);
    assert(d0->status == Status::CONSUMED);

    // t1: fanout_count=1 (just scope), fanout_refcount=1 (from scope)
    assert(d1->status == Status::CONSUMED);

    state.try_advance_ring_pointers();
    assert(state.task_ring().last_task_alive() == 2);

    printf("  state_machine_async: PASS\n");
}

static void test_diamond_dag() {
    // Diamond: t0 -> t1, t0 -> t2, t1 + t2 -> t3
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 16;
    cfg.heap_ring_capacity = 65536;
    cfg.dep_pool_capacity = 128;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    MockAsyncDispatcher disp;
    state.set_dispatcher(&disp);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target{};

    uint64_t h_in = linqu_alloc_tensor(rt, target, 256);
    linqu_scope_begin(rt);

    // t0: produce h_a
    uint64_t h_a = linqu_alloc_tensor(rt, target, 256);
    LinquParam p0[] = {linqu_make_input(h_in), linqu_make_output(h_a)};
    linqu_submit_task(rt, target, "t0.so", p0, 2);

    // t1: h_a -> h_b
    uint64_t h_b = linqu_alloc_tensor(rt, target, 256);
    LinquParam p1[] = {linqu_make_input(h_a), linqu_make_output(h_b)};
    linqu_submit_task(rt, target, "t1.so", p1, 2);

    // t2: h_a -> h_c
    uint64_t h_c = linqu_alloc_tensor(rt, target, 256);
    LinquParam p2[] = {linqu_make_input(h_a), linqu_make_output(h_c)};
    linqu_submit_task(rt, target, "t2.so", p2, 2);

    // t3: h_b + h_c -> h_d
    uint64_t h_d = linqu_alloc_tensor(rt, target, 256);
    LinquParam p3[] = {linqu_make_input(h_b), linqu_make_input(h_c), linqu_make_output(h_d)};
    linqu_submit_task(rt, target, "t3.so", p3, 3);

    auto* d0 = state.task_ring().get(0);
    auto* d1 = state.task_ring().get(1);
    auto* d2 = state.task_ring().get(2);
    auto* d3 = state.task_ring().get(3);

    // t0: fanout = 1(scope) + 2(t1,t2) = 3
    assert(d0->fanout_count == 3);
    // t1: fanin=1(t0), fanout=1(scope)+1(t3)=2
    assert(d1->fanin_count == 1);
    assert(d1->fanout_count == 2);
    // t2: fanin=1(t0), fanout=1(scope)+1(t3)=2
    assert(d2->fanin_count == 1);
    assert(d2->fanout_count == 2);
    // t3: fanin=2(t1,t2), fanout=1(scope)
    assert(d3->fanin_count == 2);
    assert(d3->fanout_count == 1);

    // Complete in topological order
    disp.complete(0); // t0 done -> t1,t2 fanin_refcount++
    assert(d1->fanin_refcount == 1);
    assert(d2->fanin_refcount == 1);

    disp.complete(1); // t1 done -> t0 fanout_refcount++, t3 fanin_refcount++
    assert(d3->fanin_refcount == 1);

    disp.complete(2); // t2 done -> t0 fanout_refcount++, t3 fanin_refcount++
    assert(d3->fanin_refcount == 2);

    disp.complete(3); // t3 done -> t1,t2 fanout_refcount++

    linqu_scope_end(rt);

    // All should be CONSUMED after scope release + completions
    assert(d0->status == Status::CONSUMED);
    assert(d1->status == Status::CONSUMED);
    assert(d2->status == Status::CONSUMED);
    assert(d3->status == Status::CONSUMED);

    state.try_advance_ring_pointers();
    assert(state.task_ring().last_task_alive() == 4);

    printf("  diamond_dag: PASS\n");
}

static void test_linear_chain() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 16;
    cfg.heap_ring_capacity = 65536;
    cfg.dep_pool_capacity = 128;
    state.init(Level::HOST, LinquCoordinate{}, cfg);

    MockAsyncDispatcher disp;
    state.set_dispatcher(&disp);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target{};

    uint64_t h = linqu_alloc_tensor(rt, target, 128);
    linqu_scope_begin(rt);

    for (int i = 0; i < 8; i++) {
        uint64_t h_out = linqu_alloc_tensor(rt, target, 128);
        LinquParam p[] = {linqu_make_input(h), linqu_make_output(h_out)};
        linqu_submit_task(rt, target, "chain.so", p, 2);
        h = h_out;
    }

    // Complete in order
    for (int i = 0; i < 8; i++) {
        disp.complete(i);
    }

    linqu_scope_end(rt);

    for (int i = 0; i < 8; i++) {
        auto* d = state.task_ring().get(i);
        assert(d->status == Status::CONSUMED);
    }

    state.try_advance_ring_pointers();
    assert(state.task_ring().last_task_alive() == 8);

    printf("  linear_chain: PASS\n");
}

static void test_no_deps_immediate_consumed() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 8;
    cfg.heap_ring_capacity = 8192;
    state.init(Level::HOST, LinquCoordinate{}, cfg);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target{};

    linqu_scope_begin(rt);
    linqu_submit_task(rt, target, "noop.so", nullptr, 0);
    linqu_scope_end(rt);

    auto* d = state.task_ring().get(0);
    // No dispatcher -> COMPLETED immediately, scope_end -> fanout_refcount reaches fanout_count
    assert(d->status == Status::CONSUMED);

    state.try_advance_ring_pointers();
    assert(state.task_ring().last_task_alive() == 1);

    printf("  no_deps_immediate_consumed: PASS\n");
}

int main() {
    printf("=== test_scheduler ===\n");
    test_fanin_fanout_counts();
    test_state_machine_async();
    test_diamond_dag();
    test_linear_chain();
    test_no_deps_immediate_consumed();
    printf("=== ALL PASS ===\n");
    return 0;
}
