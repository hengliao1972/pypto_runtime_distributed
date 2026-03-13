#include "runtime/linqu_orchestrator_state.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace linqu;

static void test_basic_orchestration() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 16;
    cfg.heap_ring_capacity = 65536;

    LinquCoordinate coord;
    coord.l5_idx = 3;
    coord.l4_idx = 2;
    coord.l3_idx = 7;
    state.init(Level::HOST, coord, cfg);

    LinquRuntime* rt = state.runtime();

    LinquCoordinate_C self = linqu_self_coord(rt);
    assert(self.l5_idx == 3);
    assert(self.l3_idx == 7);

    LinquCoordinate_C target;
    memset(&target, 0, sizeof(target));

    uint64_t h_a = linqu_alloc_tensor(rt, target, 4096);
    uint64_t h_b = linqu_alloc_tensor(rt, target, 4096);
    uint64_t h_c = linqu_alloc_tensor(rt, target, 4096);
    assert(h_a > 0 && h_b > 0 && h_c > 0);
    assert(h_a != h_b && h_b != h_c);

    linqu_scope_begin(rt);

    LinquParam params0[] = {
        linqu_make_input(h_a),
        linqu_make_input(h_b),
        linqu_make_output(h_c),
    };
    linqu_submit_task(rt, target, "kernel_add.so", params0, 3);
    assert(state.tasks_submitted() == 1);

    linqu_scope_end(rt);
    linqu_orchestration_done(rt);

    printf("  basic_orchestration: PASS\n");
}

static void test_dag_with_deps() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 16;
    cfg.heap_ring_capacity = 65536;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target;
    memset(&target, 0, sizeof(target));

    uint64_t h_a = linqu_alloc_tensor(rt, target, 1024);
    uint64_t h_b = linqu_alloc_tensor(rt, target, 1024);

    linqu_scope_begin(rt);

    // t0: c = a + b
    uint64_t h_c = linqu_alloc_tensor(rt, target, 1024);
    LinquParam p0[] = {
        linqu_make_input(h_a),
        linqu_make_input(h_b),
        linqu_make_output(h_c),
    };
    linqu_submit_task(rt, target, "add.so", p0, 3);

    linqu_scope_begin(rt);

    // t1: d = c + 1 (depends on c from t0)
    uint64_t h_d = linqu_alloc_tensor(rt, target, 1024);
    LinquParam p1[] = {
        linqu_make_input(h_c),
        linqu_make_output(h_d),
        linqu_make_scalar(1),
    };
    linqu_submit_task(rt, target, "add_scalar.so", p1, 3);

    // t2: e = c + 2 (also depends on c from t0)
    uint64_t h_e = linqu_alloc_tensor(rt, target, 1024);
    LinquParam p2[] = {
        linqu_make_input(h_c),
        linqu_make_output(h_e),
        linqu_make_scalar(2),
    };
    linqu_submit_task(rt, target, "add_scalar.so", p2, 3);

    // t3: f = d * e
    uint64_t h_f = linqu_alloc_tensor(rt, target, 1024);
    LinquParam p3[] = {
        linqu_make_input(h_d),
        linqu_make_input(h_e),
        linqu_make_output(h_f),
    };
    linqu_submit_task(rt, target, "mul.so", p3, 3);

    assert(state.tasks_submitted() == 4);

    auto r = state.tensor_map().lookup(h_f);
    assert(r.found && r.producer_task_id == 3);

    auto r_c = state.tensor_map().lookup(h_c);
    assert(r_c.found && r_c.producer_task_id == 0);

    linqu_scope_end(rt);
    linqu_scope_end(rt);

    linqu_orchestration_done(rt);

    printf("  dag_with_deps: PASS\n");
}

static void test_dispatch_callback() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 8;
    cfg.heap_ring_capacity = 8192;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    std::vector<std::string> dispatched;
    state.set_dispatch_fn([&](int32_t tid, const LinquCoordinate& tgt,
                              const std::string& so,
                              LinquParam*, int) {
        dispatched.push_back(so);
    });

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target;
    memset(&target, 0, sizeof(target));

    linqu_scope_begin(rt);
    linqu_submit_task(rt, target, "a.so", nullptr, 0);
    linqu_submit_task(rt, target, "b.so", nullptr, 0);
    linqu_submit_task(rt, target, "c.so", nullptr, 0);
    linqu_scope_end(rt);

    assert(dispatched.size() == 3);
    assert(dispatched[0] == "a.so");
    assert(dispatched[1] == "b.so");
    assert(dispatched[2] == "c.so");

    printf("  dispatch_callback: PASS\n");
}

static void test_linqu_scope_macro() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 8;
    cfg.heap_ring_capacity = 8192;
    state.init(Level::HOST, LinquCoordinate{}, cfg);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target;
    memset(&target, 0, sizeof(target));

    LINQU_SCOPE(rt) {
        linqu_submit_task(rt, target, "t0.so", nullptr, 0);
        LINQU_SCOPE(rt) {
            linqu_submit_task(rt, target, "t1.so", nullptr, 0);
        }
        linqu_submit_task(rt, target, "t2.so", nullptr, 0);
    }

    assert(state.tasks_submitted() == 3);
    assert(state.scope_stack().depth() == -1);

    printf("  linqu_scope_macro: PASS\n");
}

int main() {
    printf("=== test_orchestrator_state ===\n");
    test_basic_orchestration();
    test_dag_with_deps();
    test_dispatch_callback();
    test_linqu_scope_macro();
    printf("=== ALL PASS ===\n");
    return 0;
}
