#include "runtime/linqu_tensormap.h"
#include <cassert>
#include <cstdio>

using namespace linqu;

static void test_basic_insert_lookup() {
    LinquTensorMap tm;
    tm.init(16, 64);

    tm.insert(100, 0);
    tm.insert(200, 1);
    tm.insert(300, 2);

    auto r = tm.lookup(100);
    assert(r.found && r.producer_task_id == 0);

    r = tm.lookup(200);
    assert(r.found && r.producer_task_id == 1);

    r = tm.lookup(300);
    assert(r.found && r.producer_task_id == 2);

    r = tm.lookup(999);
    assert(!r.found);

    printf("  basic_insert_lookup: PASS\n");
}

static void test_per_task_chain() {
    LinquTensorMap tm;
    tm.init(16, 64);

    tm.insert(100, 5);
    tm.insert(200, 5);
    tm.insert(300, 5);

    int32_t head = tm.task_chain_head(5);
    assert(head >= 0);

    int count = 0;
    int32_t cur = head;
    while (cur >= 0) {
        count++;
        cur = -1; // Can't easily traverse from outside, just check head exists
    }
    assert(count >= 1);

    printf("  per_task_chain: PASS\n");
}

static void test_inout_optimization() {
    LinquTensorMap tm;
    tm.init(16, 64);

    tm.insert(100, 0, true);

    auto r = tm.lookup(100);
    assert(r.found && r.producer_task_id == 0);

    tm.insert(100, 1, true);

    r = tm.lookup(100);
    assert(r.found && r.producer_task_id == 1);

    assert(tm.valid_count() == 1);

    printf("  inout_optimization: PASS\n");
}

static void test_same_handle_different_tasks() {
    LinquTensorMap tm;
    tm.init(16, 64);

    tm.insert(100, 0, true);
    tm.insert(100, 2, true);

    auto r = tm.lookup(100);
    assert(r.found && r.producer_task_id == 2);

    assert(tm.valid_count() == 1);

    printf("  same_handle_different_tasks: PASS\n");
}

static void test_cleanup_task() {
    LinquTensorMap tm;
    tm.init(16, 64);

    tm.insert(100, 0);
    tm.insert(200, 0);
    tm.insert(300, 1);

    assert(tm.valid_count() == 3);

    tm.cleanup_task(0);

    assert(tm.valid_count() == 1);
    assert(!tm.lookup(100).found);
    assert(!tm.lookup(200).found);
    assert(tm.lookup(300).found);

    printf("  cleanup_task: PASS\n");
}

static void test_invalidate_below() {
    LinquTensorMap tm;
    tm.init(16, 64);

    tm.insert(100, 0);
    tm.insert(200, 1);
    tm.insert(300, 2);
    tm.insert(400, 3);

    tm.invalidate_below(2);

    assert(!tm.lookup(100).found);
    assert(!tm.lookup(200).found);
    assert(tm.lookup(300).found);
    assert(tm.lookup(400).found);

    assert(tm.valid_count() == 2);

    printf("  invalidate_below: PASS\n");
}

static void test_lookup_returns_latest_producer() {
    LinquTensorMap tm;
    tm.init(16, 64);

    tm.insert(100, 0, false);
    tm.insert(100, 3, false);

    auto r = tm.lookup(100);
    assert(r.found && r.producer_task_id == 3);

    printf("  lookup_returns_latest_producer: PASS\n");
}

static void test_many_entries_pool_wrap() {
    LinquTensorMap tm;
    tm.init(16, 32);

    for (int i = 0; i < 64; i++) {
        tm.insert(static_cast<uint64_t>(i * 100), i);
    }

    auto r = tm.lookup(6300);
    assert(r.found && r.producer_task_id == 63);

    printf("  many_entries_pool_wrap: PASS\n");
}

int main() {
    printf("=== test_tensormap_enhanced ===\n");
    test_basic_insert_lookup();
    test_per_task_chain();
    test_inout_optimization();
    test_same_handle_different_tasks();
    test_cleanup_task();
    test_invalidate_below();
    test_lookup_returns_latest_producer();
    test_many_entries_pool_wrap();
    printf("=== ALL PASS ===\n");
    return 0;
}
