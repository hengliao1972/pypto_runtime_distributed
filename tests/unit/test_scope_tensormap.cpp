#include "runtime/linqu_scope.h"
#include "runtime/linqu_tensormap.h"
#include "ring/linqu_task_ring.h"
#include <cassert>
#include <cstdio>

using namespace linqu;

static LinquTaskRing g_task_ring;

static LinquTaskDescriptor* get_desc(int32_t task_id, void*) {
    return g_task_ring.get(task_id);
}

static void test_scope_basic() {
    g_task_ring.init(16);
    LinquScopeStack ss;
    ss.init(8, 256);

    assert(ss.depth() == -1);

    ss.scope_begin();
    assert(ss.depth() == 0);

    int32_t t0 = g_task_ring.alloc();
    int32_t t1 = g_task_ring.alloc();
    ss.add_task(t0);
    ss.add_task(t1);
    assert(ss.tasks_in_current_scope() == 2);

    ss.scope_end(get_desc, nullptr);
    assert(ss.depth() == -1);

    assert(g_task_ring.get(t0)->fanout_refcount == 1);
    assert(g_task_ring.get(t1)->fanout_refcount == 1);

    printf("  scope_basic: PASS\n");
}

static void test_scope_nested() {
    g_task_ring.init(16);
    LinquScopeStack ss;
    ss.init(8, 256);

    ss.scope_begin();  // depth 0
    int32_t outer0 = g_task_ring.alloc();
    int32_t outer1 = g_task_ring.alloc();
    ss.add_task(outer0);
    ss.add_task(outer1);

    ss.scope_begin();  // depth 1
    int32_t inner0 = g_task_ring.alloc();
    int32_t inner1 = g_task_ring.alloc();
    ss.add_task(inner0);
    ss.add_task(inner1);

    ss.scope_end(get_desc, nullptr);  // exit depth 1
    assert(ss.depth() == 0);
    assert(g_task_ring.get(inner0)->fanout_refcount == 1);
    assert(g_task_ring.get(inner1)->fanout_refcount == 1);
    assert(g_task_ring.get(outer0)->fanout_refcount == 0);
    assert(g_task_ring.get(outer1)->fanout_refcount == 0);

    ss.scope_end(get_desc, nullptr);  // exit depth 0
    assert(ss.depth() == -1);
    assert(g_task_ring.get(outer0)->fanout_refcount == 1);
    assert(g_task_ring.get(outer1)->fanout_refcount == 1);

    printf("  scope_nested: PASS\n");
}

static void test_scope_pl_free() {
    g_task_ring.init(16);
    LinquScopeStack ss;
    ss.init(8, 256);

    ss.scope_begin();
    int32_t t0 = g_task_ring.alloc();
    int32_t t1 = g_task_ring.alloc();
    ss.add_task(t0);
    ss.add_task(t1);

    g_task_ring.get(t0)->task_freed = true;
    g_task_ring.get(t0)->fanout_refcount = 1;  // pl.free applied token

    ss.scope_end(get_desc, nullptr);

    assert(g_task_ring.get(t0)->fanout_refcount == 1);  // NOT incremented (skipped)
    assert(g_task_ring.get(t1)->fanout_refcount == 1);   // incremented

    printf("  scope_pl_free: PASS\n");
}

static void test_tensormap_basic() {
    LinquTensorMap tm;
    tm.init(16, 64);

    tm.insert(100, 0);
    tm.insert(200, 1);
    tm.insert(300, 2);

    auto r1 = tm.lookup(100);
    assert(r1.found && r1.producer_task_id == 0);

    auto r2 = tm.lookup(200);
    assert(r2.found && r2.producer_task_id == 1);

    auto r3 = tm.lookup(999);
    assert(!r3.found);

    assert(tm.valid_count() == 3);

    printf("  tensormap_basic: PASS\n");
}

static void test_tensormap_invalidation() {
    LinquTensorMap tm;
    tm.init(16, 64);

    tm.insert(100, 0);
    tm.insert(200, 1);
    tm.insert(300, 5);

    tm.invalidate_below(2);

    auto r1 = tm.lookup(100);
    assert(!r1.found);  // task 0 < 2

    auto r2 = tm.lookup(200);
    assert(!r2.found);  // task 1 < 2

    auto r3 = tm.lookup(300);
    assert(r3.found && r3.producer_task_id == 5);  // task 5 >= 2

    printf("  tensormap_invalidation: PASS\n");
}

static void test_tensormap_overwrite() {
    LinquTensorMap tm;
    tm.init(16, 64);

    tm.insert(100, 0);
    tm.insert(100, 5);

    auto r = tm.lookup(100);
    assert(r.found && r.producer_task_id == 5);

    printf("  tensormap_overwrite: PASS\n");
}

int main() {
    printf("=== test_scope_tensormap ===\n");
    test_scope_basic();
    test_scope_nested();
    test_scope_pl_free();
    test_tensormap_basic();
    test_tensormap_invalidation();
    test_tensormap_overwrite();
    printf("=== ALL PASS ===\n");
    return 0;
}
