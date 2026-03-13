#include "runtime/linqu_orchestrator_state.h"
#include "runtime/linqu_dispatcher.h"
#include <cassert>
#include <cstdio>
#include <cstring>

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
        notify_completion(r);
    }

    std::vector<int32_t> dispatched_;
};

static void test_task_ring_retirement() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 8;
    cfg.heap_ring_capacity = 65536;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target{};

    linqu_scope_begin(rt);
    for (int i = 0; i < 4; i++) {
        linqu_submit_task(rt, target, "task.so", nullptr, 0);
    }
    linqu_scope_end(rt);

    assert(state.task_ring().last_task_alive() == 4);
    assert(state.task_ring().active_count() == 0);

    linqu_scope_begin(rt);
    for (int i = 0; i < 4; i++) {
        linqu_submit_task(rt, target, "task.so", nullptr, 0);
    }
    linqu_scope_end(rt);

    assert(state.task_ring().last_task_alive() == 8);

    printf("  task_ring_retirement: PASS\n");
}

static void test_heap_retirement() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 8;
    cfg.heap_ring_capacity = 4096;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target{};

    size_t initial_avail = state.heap_ring().available();

    linqu_scope_begin(rt);
    uint64_t h1 = linqu_alloc_tensor(rt, target, 1024);
    LinquParam p1[] = {linqu_make_output(h1)};
    linqu_submit_task(rt, target, "alloc.so", p1, 1);
    linqu_scope_end(rt);

    size_t after_retire_avail = state.heap_ring().available();
    assert(after_retire_avail >= initial_avail - 128);

    printf("  heap_retirement: PASS\n");
}

static void test_ring_recycles_slots() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 4;
    cfg.heap_ring_capacity = 65536;
    cfg.dep_pool_capacity = 256;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target{};

    for (int batch = 0; batch < 5; batch++) {
        linqu_scope_begin(rt);
        for (int i = 0; i < 4; i++) {
            linqu_submit_task(rt, target, "task.so", nullptr, 0);
        }
        linqu_scope_end(rt);
    }

    assert(state.tasks_submitted() == 20);
    assert(state.task_ring().active_count() == 0);

    printf("  ring_recycles_slots: PASS\n");
}

static void test_async_ring_retirement() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 4;
    cfg.heap_ring_capacity = 65536;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    MockAsyncDispatcher disp;
    state.set_dispatcher(&disp);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target{};

    linqu_scope_begin(rt);
    for (int i = 0; i < 4; i++) {
        linqu_submit_task(rt, target, "task.so", nullptr, 0);
    }

    assert(state.task_ring().active_count() == 4);
    assert(!state.task_ring().has_space());

    for (int i = 0; i < 4; i++) {
        disp.complete(i);
    }
    linqu_scope_end(rt);

    state.try_advance_ring_pointers();
    assert(state.task_ring().last_task_alive() == 4);
    assert(state.task_ring().has_space());

    linqu_scope_begin(rt);
    for (int i = 0; i < 4; i++) {
        linqu_submit_task(rt, target, "task.so", nullptr, 0);
    }
    for (int i = 4; i < 8; i++) {
        disp.complete(i);
    }
    linqu_scope_end(rt);

    state.try_advance_ring_pointers();
    assert(state.task_ring().last_task_alive() == 8);

    printf("  async_ring_retirement: PASS\n");
}

static void test_partial_retirement() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 8;
    cfg.heap_ring_capacity = 65536;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    MockAsyncDispatcher disp;
    state.set_dispatcher(&disp);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target{};

    linqu_scope_begin(rt);
    for (int i = 0; i < 6; i++) {
        linqu_submit_task(rt, target, "task.so", nullptr, 0);
    }

    // Complete tasks 0,1,2 but not 3,4,5
    disp.complete(0);
    disp.complete(1);
    disp.complete(2);

    linqu_scope_end(rt);
    state.try_advance_ring_pointers();

    // Only 0,1,2 are fully CONSUMED (completed + scope released),
    // but 3,4,5 are still RUNNING, so retirement stops at 3
    assert(state.task_ring().last_task_alive() == 3);

    // Now complete the rest
    disp.complete(3);
    disp.complete(4);
    disp.complete(5);
    state.try_advance_ring_pointers();
    assert(state.task_ring().last_task_alive() == 6);

    printf("  partial_retirement: PASS\n");
}

int main() {
    printf("=== test_ring_retirement ===\n");
    test_task_ring_retirement();
    test_heap_retirement();
    test_ring_recycles_slots();
    test_async_ring_retirement();
    test_partial_retirement();
    printf("=== ALL PASS ===\n");
    return 0;
}
