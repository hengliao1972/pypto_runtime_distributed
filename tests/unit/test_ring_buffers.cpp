#include "ring/linqu_heap_ring.h"
#include "ring/linqu_task_ring.h"
#include "ring/linqu_dep_pool.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace linqu;

static void test_heap_ring_basic() {
    LinquHeapRing hr;
    hr.init(32768);  // 32 KB
    assert(hr.capacity() == 32768);
    assert(hr.used() == 0);
    assert(hr.available() == 32768);

    void* p1 = hr.alloc(4096);
    assert(p1 != nullptr);
    assert(hr.used() == 4096);

    void* p2 = hr.alloc(4096);
    assert(p2 != nullptr);
    assert(hr.used() == 8192);

    hr.retire_to(4096);
    assert(hr.used() == 4096);
    assert(hr.available() == 32768 - 4096);

    hr.reset();
    assert(hr.used() == 0);

    printf("  heap_ring_basic: PASS\n");
}

static void test_heap_ring_fill_and_retire() {
    LinquHeapRing hr;
    hr.init(32768);

    for (int i = 0; i < 8; i++) {
        void* p = hr.alloc(4096);
        assert(p != nullptr);
        memset(p, i, 4096);
    }
    assert(hr.used() == 32768);

    void* fail = nullptr;
    bool ok = hr.try_alloc(1, &fail);
    assert(!ok);

    hr.retire_to(32768);
    assert(hr.used() == 0);

    void* p = hr.alloc(4096);
    assert(p != nullptr);

    printf("  heap_ring_fill_and_retire: PASS\n");
}

static void test_task_ring_basic() {
    LinquTaskRing tr;
    tr.init(8);  // window of 8
    assert(tr.window_size() == 8);
    assert(tr.active_count() == 0);
    assert(tr.has_space());

    int32_t id0 = tr.alloc();
    assert(id0 == 0);
    assert(tr.active_count() == 1);

    auto* desc = tr.get(id0);
    assert(desc != nullptr);
    assert(desc->status == LinquTaskDescriptor::Status::PENDING);

    for (int i = 1; i < 8; i++) {
        int32_t id = tr.alloc();
        assert(id == i);
    }
    assert(tr.active_count() == 8);
    assert(!tr.has_space());

    assert(tr.try_alloc() == -1);

    tr.set_last_task_alive(4);
    assert(tr.active_count() == 4);
    assert(tr.has_space());

    int32_t id8 = tr.alloc();
    assert(id8 == 8);

    printf("  task_ring_basic: PASS\n");
}

static void test_task_ring_wrap() {
    LinquTaskRing tr;
    tr.init(4);

    for (int i = 0; i < 4; i++) tr.alloc();
    assert(!tr.has_space());
    tr.set_last_task_alive(4);
    assert(tr.active_count() == 0);

    for (int i = 0; i < 4; i++) {
        int32_t id = tr.alloc();
        assert(id == 4 + i);
        auto* d = tr.get(id);
        d->kernel_so = "test.so";
    }
    auto* d = tr.get(6);
    assert(d->kernel_so == "test.so");

    printf("  task_ring_wrap: PASS\n");
}

static void test_dep_pool_basic() {
    LinquDepListPool pool;
    pool.init(64);
    assert(pool.used() == 0);

    int32_t head = -1;
    head = pool.prepend(head, 10);
    head = pool.prepend(head, 20);
    head = pool.prepend(head, 30);

    assert(pool.count(head) == 3);

    auto* e = pool.get(head);
    assert(e->task_id == 30);
    e = pool.get(e->next);
    assert(e->task_id == 20);
    e = pool.get(e->next);
    assert(e->task_id == 10);
    assert(e->next == -1);

    pool.reset();
    assert(pool.used() == 0);

    printf("  dep_pool_basic: PASS\n");
}

int main() {
    printf("=== test_ring_buffers ===\n");
    test_heap_ring_basic();
    test_heap_ring_fill_and_retire();
    test_task_ring_basic();
    test_task_ring_wrap();
    test_dep_pool_basic();
    printf("=== ALL PASS ===\n");
    return 0;
}
