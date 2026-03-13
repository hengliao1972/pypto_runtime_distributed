#include "runtime/linqu_orchestrator_state.h"
#include "runtime/linqu_dispatcher.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace linqu;

class MockAsyncDispatcher : public LinquDispatcher {
public:
    bool dispatch(int32_t task_id, const LinquCoordinate&,
                  const std::string& kernel_so, LinquParam*, int) override {
        dispatched_ids_.push_back(task_id);
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
};

static void test_task_stays_running_until_complete() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 16;
    cfg.heap_ring_capacity = 65536;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    MockAsyncDispatcher disp;
    state.set_dispatcher(&disp);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target;
    memset(&target, 0, sizeof(target));

    linqu_scope_begin(rt);

    linqu_submit_task(rt, target, "kernel.so", nullptr, 0);
    assert(disp.dispatched_ids_.size() == 1);

    auto* desc = state.task_ring().get(0);
    assert(desc->status == LinquTaskDescriptor::Status::RUNNING);

    disp.simulate_complete(0);
    assert(desc->status == LinquTaskDescriptor::Status::COMPLETED ||
           desc->status == LinquTaskDescriptor::Status::CONSUMED);

    linqu_scope_end(rt);

    printf("  task_stays_running_until_complete: PASS\n");
}

static void test_completion_callback_wired() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 16;
    cfg.heap_ring_capacity = 65536;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    MockAsyncDispatcher disp;
    state.set_dispatcher(&disp);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target;
    memset(&target, 0, sizeof(target));

    linqu_scope_begin(rt);
    linqu_submit_task(rt, target, "k1.so", nullptr, 0);
    linqu_submit_task(rt, target, "k2.so", nullptr, 0);
    linqu_submit_task(rt, target, "k3.so", nullptr, 0);
    linqu_scope_end(rt);

    for (int i = 0; i < 3; i++) {
        auto* d = state.task_ring().get(i);
        assert(d->status == LinquTaskDescriptor::Status::RUNNING);
    }

    disp.simulate_complete(0);
    disp.simulate_complete(1);
    disp.simulate_complete(2);

    state.try_advance_ring_pointers();

    assert(state.task_ring().last_task_alive() == 3);

    printf("  completion_callback_wired: PASS\n");
}

static void test_dispatch_fn_completes_immediately() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 16;
    cfg.heap_ring_capacity = 65536;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    std::vector<int32_t> dispatched;
    state.set_dispatch_fn([&](int32_t tid, const LinquCoordinate&,
                              const std::string&, LinquParam*, int) {
        dispatched.push_back(tid);
    });

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target;
    memset(&target, 0, sizeof(target));

    linqu_scope_begin(rt);
    linqu_submit_task(rt, target, "k.so", nullptr, 0);
    linqu_scope_end(rt);

    auto* desc = state.task_ring().get(0);
    assert(desc->status == LinquTaskDescriptor::Status::CONSUMED);

    state.try_advance_ring_pointers();
    assert(state.task_ring().last_task_alive() == 1);

    printf("  dispatch_fn_completes_immediately: PASS\n");
}

static void test_no_dispatcher_completes_immediately() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 8;
    cfg.heap_ring_capacity = 8192;
    state.init(Level::HOST, LinquCoordinate{}, cfg);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target;
    memset(&target, 0, sizeof(target));

    linqu_scope_begin(rt);
    linqu_submit_task(rt, target, "k.so", nullptr, 0);
    linqu_scope_end(rt);

    auto* desc = state.task_ring().get(0);
    assert(desc->status == LinquTaskDescriptor::Status::CONSUMED);

    printf("  no_dispatcher_completes_immediately: PASS\n");
}

int main() {
    printf("=== test_completion_wiring ===\n");
    test_task_stays_running_until_complete();
    test_completion_callback_wired();
    test_dispatch_fn_completes_immediately();
    test_no_dispatcher_completes_immediately();
    printf("=== ALL PASS ===\n");
    return 0;
}
