#include "core/level.h"
#include "core/coordinate.h"
#include "core/task_key.h"
#include <cassert>
#include <unordered_map>
#include <map>
#include <cstdio>

using namespace linqu;

static void test_level_enum() {
    static_assert(level_value(Level::CORE) == 0, "");
    static_assert(level_value(Level::AIV) == 0, "");
    static_assert(level_value(Level::CHIP_DIE) == 1, "");
    static_assert(level_value(Level::CHIP) == 2, "");
    static_assert(level_value(Level::PROCESSOR) == 2, "");
    static_assert(level_value(Level::UMA) == 2, "");
    static_assert(level_value(Level::HOST) == 3, "");
    static_assert(level_value(Level::NODE) == 3, "");
    static_assert(level_value(Level::POD) == 4, "");
    static_assert(level_value(Level::CLUSTER_0) == 4, "");
    static_assert(level_value(Level::CLOS1) == 5, "");
    static_assert(level_value(Level::CLUSTER_1) == 5, "");
    static_assert(level_value(Level::CLOS2) == 6, "");
    static_assert(level_value(Level::CLUSTER_2) == 6, "");

    assert(std::string(level_name(Level::CORE)) == "CORE");
    assert(std::string(level_name(Level::HOST)) == "HOST");
    assert(std::string(level_name(Level::CLOS2)) == "CLOS2");

    printf("  level_enum: PASS\n");
}

static void test_coordinate_basics() {
    LinquCoordinate c;
    assert(c.l6_idx == 0);
    assert(c.l0_idx == 0);
    assert(c == LinquCoordinate{});

    LinquCoordinate a{};
    a.l5_idx = 3;
    a.l4_idx = 2;
    a.l3_idx = 7;

    assert(a != c);
    assert(c < a);

    assert(a.to_string() == "(l6=0,l5=3,l4=2,l3=7,l2=0,l1=0,l0=0)");
    assert(a.to_path() == "L6_0/L5_3/L4_2/L3_7/L2_0/L1_0/L0_0");

    assert(a.index_at(Level::CLOS1) == 3);
    assert(a.index_at(Level::POD) == 2);
    assert(a.index_at(Level::HOST) == 7);

    printf("  coordinate_basics: PASS\n");
}

static void test_coordinate_from_env() {
    setenv("LINQU_L5", "12", 1);
    setenv("LINQU_L4", "3", 1);
    setenv("LINQU_L3", "15", 1);
    auto c = LinquCoordinate::from_env();
    assert(c.l5_idx == 12);
    assert(c.l4_idx == 3);
    assert(c.l3_idx == 15);
    assert(c.l6_idx == 0);
    unsetenv("LINQU_L5");
    unsetenv("LINQU_L4");
    unsetenv("LINQU_L3");

    printf("  coordinate_from_env: PASS\n");
}

static void test_coordinate_ordering() {
    LinquCoordinate a{}, b{};
    a.l5_idx = 1; a.l4_idx = 2;
    b.l5_idx = 1; b.l4_idx = 3;
    assert(a < b);

    std::map<LinquCoordinate, int> m;
    m[a] = 1;
    m[b] = 2;
    assert(m.size() == 2);
    assert(m[a] == 1);

    printf("  coordinate_ordering: PASS\n");
}

static void test_task_key() {
    TaskKey k;
    k.logical_system = "vcluster";
    k.coord.l5_idx = 3;
    k.coord.l4_idx = 2;
    k.coord.l3_idx = 7;
    k.scope_depth = 2;
    k.task_id = 42;

    assert(k.to_string() == "vcluster:(0,3,2,7):d2:t42");

    TaskKey k2 = k;
    assert(k == k2);
    k2.task_id = 43;
    assert(k < k2);

    printf("  task_key: PASS\n");
}

static void test_task_key_hash() {
    std::unordered_map<TaskKey, int> m;
    TaskKey k;
    k.logical_system = "sys";
    k.task_id = 1;
    m[k] = 10;

    TaskKey k2 = k;
    k2.task_id = 2;
    m[k2] = 20;

    assert(m.size() == 2);
    assert(m[k] == 10);
    assert(m[k2] == 20);

    printf("  task_key_hash: PASS\n");
}

int main() {
    printf("=== test_core_identity ===\n");
    test_level_enum();
    test_coordinate_basics();
    test_coordinate_from_env();
    test_coordinate_ordering();
    test_task_key();
    test_task_key_hash();
    printf("=== ALL PASS ===\n");
    return 0;
}
