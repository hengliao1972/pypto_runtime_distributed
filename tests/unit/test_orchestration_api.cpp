#include "runtime/linqu_orchestration_api.h"
#include <cassert>
#include <cstdio>
#include <cstring>

static int submit_count = 0;
static int scope_begin_count = 0;
static int scope_end_count = 0;
static int done_count = 0;

static void mock_submit(LinquRuntime*, LinquCoordinate_C,
                         const char*, LinquParam*, int) {
    submit_count++;
}
static void mock_scope_begin(LinquRuntime*) { scope_begin_count++; }
static void mock_scope_end(LinquRuntime*) { scope_end_count++; }
static uint64_t mock_alloc(LinquRuntime*, LinquCoordinate_C, size_t s) {
    return s;
}
static void mock_free(LinquRuntime*, uint64_t) {}
static void mock_done(LinquRuntime*) { done_count++; }
static uint64_t mock_reg(LinquRuntime*, LinquCoordinate_C, const void*, size_t s) {
    return s;
}
static LinquPeerList mock_peers(LinquRuntime*, uint8_t) {
    LinquPeerList pl; pl.peers = nullptr; pl.count = 0; return pl;
}
static LinquCoordinate_C mock_self(LinquRuntime*) {
    LinquCoordinate_C c;
    memset(&c, 0, sizeof(c));
    c.l5_idx = 7;
    return c;
}
static void mock_wait(LinquRuntime*) {}
static void mock_snap(LinquRuntime*, const char*) {}
static void mock_log(LinquRuntime*, const char*, ...) {}
static void* mock_pool(LinquRuntime*) { return nullptr; }

static void test_ops_table_wrappers() {
    LinquRuntimeOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.submit_task = mock_submit;
    ops.scope_begin = mock_scope_begin;
    ops.scope_end = mock_scope_end;
    ops.alloc_tensor = mock_alloc;
    ops.free_tensor = mock_free;
    ops.orchestration_done = mock_done;
    ops.reg_data = mock_reg;
    ops.query_peers = mock_peers;
    ops.self_coord = mock_self;
    ops.wait_all = mock_wait;
    ops.dump_ring_snapshot = mock_snap;
    ops.log_error = mock_log;
    ops.log_warn = mock_log;
    ops.log_info = mock_log;
    ops.log_debug = mock_log;
    ops.get_tensor_pool = mock_pool;

    LinquRuntime rt;
    rt.ops = &ops;

    LinquCoordinate_C target;
    memset(&target, 0, sizeof(target));

    linqu_submit_task(&rt, target, "kernel.so", nullptr, 0);
    linqu_submit_task(&rt, target, "kernel.so", nullptr, 0);
    assert(submit_count == 2);

    uint64_t h = linqu_alloc_tensor(&rt, target, 4096);
    assert(h == 4096);

    linqu_free_tensor(&rt, h);
    linqu_orchestration_done(&rt);
    assert(done_count == 1);

    LinquCoordinate_C self = linqu_self_coord(&rt);
    assert(self.l5_idx == 7);

    LinquPeerList pl = linqu_query_peers(&rt, LINQU_LEVEL_HOST);
    assert(pl.count == 0);

    linqu_wait_all(&rt);

    printf("  ops_table_wrappers: PASS\n");
}

static void test_param_factories() {
    LinquParam p_in = linqu_make_input(100);
    assert(p_in.type == LINQU_PARAM_INPUT);
    assert(p_in.handle == 100);
    assert(p_in.scalar_value == 0);

    LinquParam p_out = linqu_make_output(200);
    assert(p_out.type == LINQU_PARAM_OUTPUT);
    assert(p_out.handle == 200);

    LinquParam p_io = linqu_make_inout(300);
    assert(p_io.type == LINQU_PARAM_INOUT);
    assert(p_io.handle == 300);

    LinquParam p_s = linqu_make_scalar(42);
    assert(p_s.type == LINQU_PARAM_SCALAR);
    assert(p_s.scalar_value == 42);
    assert(p_s.handle == 0);

    printf("  param_factories: PASS\n");
}

static void test_scope_guard() {
    LinquRuntimeOps ops;
    memset(&ops, 0, sizeof(ops));
    ops.scope_begin = mock_scope_begin;
    ops.scope_end = mock_scope_end;

    LinquRuntime rt;
    rt.ops = &ops;

    scope_begin_count = 0;
    scope_end_count = 0;

    {
        LINQU_SCOPE(&rt) {
            assert(scope_begin_count == 1);
            assert(scope_end_count == 0);
        }
    }
    assert(scope_begin_count == 1);
    assert(scope_end_count == 1);

    LINQU_SCOPE(&rt) {
        LINQU_SCOPE(&rt) {
            assert(scope_begin_count == 3);
        }
        assert(scope_end_count == 2);
    }
    assert(scope_begin_count == 3);
    assert(scope_end_count == 3);

    printf("  scope_guard: PASS\n");
}

static void test_config_struct() {
    LinquOrchConfig cfg;
    cfg.level = LINQU_LEVEL_POD;
    cfg.expected_arg_count = 4;
    assert(cfg.level == 4);
    assert(cfg.expected_arg_count == 4);

    printf("  config_struct: PASS\n");
}

int main() {
    printf("=== test_orchestration_api ===\n");
    test_ops_table_wrappers();
    test_param_factories();
    test_scope_guard();
    test_config_struct();
    printf("=== ALL PASS ===\n");
    return 0;
}
