#include "runtime/linqu_orchestrator_state.h"
#include "runtime/linqu_dispatcher.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>

using namespace linqu;

class DelayedDispatcher : public LinquDispatcher {
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

static void test_task_ring_backpressure() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 4;
    cfg.heap_ring_capacity = 65536;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    DelayedDispatcher disp;
    state.set_dispatcher(&disp);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target{};

    // Submit 4 tasks to fill the ring
    linqu_scope_begin(rt);
    for (int i = 0; i < 4; i++) {
        linqu_submit_task(rt, target, "k.so", nullptr, 0);
    }
    assert(disp.dispatched_.size() == 4);
    assert(!state.task_ring().has_space());

    // Complete and scope_end to retire
    for (int i = 0; i < 4; i++) {
        disp.complete(i);
    }
    linqu_scope_end(rt);

    assert(state.task_ring().has_space());

    // Now submit another batch using freed slots
    linqu_scope_begin(rt);
    for (int i = 0; i < 4; i++) {
        linqu_submit_task(rt, target, "k.so", nullptr, 0);
    }
    for (int i = 4; i < 8; i++) {
        disp.complete(i);
    }
    linqu_scope_end(rt);

    assert(state.tasks_submitted() == 8);

    printf("  task_ring_backpressure: PASS\n");
}

static void test_backpressure_with_thread() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 4;
    cfg.heap_ring_capacity = 65536;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    DelayedDispatcher disp;
    state.set_dispatcher(&disp);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target{};

    linqu_scope_begin(rt);
    for (int i = 0; i < 4; i++) {
        linqu_submit_task(rt, target, "k.so", nullptr, 0);
    }
    assert(!state.task_ring().has_space());

    // Complete 2 tasks from a background thread, freeing 2 slots
    std::thread completer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        disp.complete(0);
        disp.complete(1);
    });

    // Wait for the rest to complete before scope_end
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    disp.complete(2);
    disp.complete(3);
    linqu_scope_end(rt);

    completer.join();

    // Now submit more tasks using freed slots
    linqu_scope_begin(rt);
    linqu_submit_task(rt, target, "k.so", nullptr, 0);
    linqu_submit_task(rt, target, "k.so", nullptr, 0);
    for (int i = 4; i < 6; i++) {
        disp.complete(i);
    }
    linqu_scope_end(rt);

    assert(state.tasks_submitted() == 6);

    printf("  backpressure_with_thread: PASS\n");
}

static void test_many_batches_no_deadlock() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 4;
    cfg.heap_ring_capacity = 65536;
    state.init(Level::HOST, LinquCoordinate{}, cfg);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target{};

    for (int batch = 0; batch < 100; batch++) {
        linqu_scope_begin(rt);
        for (int i = 0; i < 4; i++) {
            linqu_submit_task(rt, target, "k.so", nullptr, 0);
        }
        linqu_scope_end(rt);
    }

    assert(state.tasks_submitted() == 400);
    assert(state.task_ring().active_count() == 0);

    printf("  many_batches_no_deadlock: PASS\n");
}

static void test_heap_backpressure() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 16;
    cfg.heap_ring_capacity = 4096;
    state.init(Level::HOST, LinquCoordinate{}, cfg);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target{};

    for (int batch = 0; batch < 10; batch++) {
        linqu_scope_begin(rt);
        uint64_t h = linqu_alloc_tensor(rt, target, 1024);
        LinquParam p[] = {linqu_make_output(h)};
        linqu_submit_task(rt, target, "k.so", p, 1);
        linqu_scope_end(rt);
    }

    assert(state.tasks_submitted() == 10);

    printf("  heap_backpressure: PASS\n");
}

int main() {
    printf("=== test_backpressure ===\n");
    test_task_ring_backpressure();
    test_backpressure_with_thread();
    test_many_batches_no_deadlock();
    test_heap_backpressure();
    printf("=== ALL PASS ===\n");
    return 0;
}
