#include "core/coordinate.h"
#include "core/task_key.h"
#include "core/level.h"
#include "core/node_identity.h"
#include "core/coordinate_mapper.h"
#include "transport/linqu_header.h"
#include <cassert>
#include <cstdio>
#include <cstring>

using namespace linqu;

static void test_coordinate_full_7_levels() {
    LinquCoordinate c;
    c.l6_idx = 2;
    c.l5_idx = 5;
    c.l4_idx = 10;
    c.l3_idx = 100;
    c.l2_idx = 3;
    c.l1_idx = 1;
    c.l0_idx = 7;

    assert(c.l6_idx == 2);
    assert(c.l5_idx == 5);
    assert(c.l4_idx == 10);
    assert(c.l3_idx == 100);
    assert(c.l2_idx == 3);
    assert(c.l1_idx == 1);
    assert(c.l0_idx == 7);

    std::string s = c.to_string();
    assert(!s.empty());

    printf("  PASS: coordinate with all 7 levels populated\n");
}

static void test_task_key_full_coordinate() {
    TaskKey k;
    k.coord.l6_idx = 1;
    k.coord.l5_idx = 3;
    k.coord.l4_idx = 7;
    k.coord.l3_idx = 15;
    k.coord.l2_idx = 2;
    k.coord.l1_idx = 0;
    k.coord.l0_idx = 5;
    k.scope_depth = 4;
    k.task_id = 42;

    TaskKey k2 = k;
    assert(k == k2);

    k2.task_id = 43;
    assert(!(k == k2));

    auto h = std::hash<TaskKey>{}(k);
    auto h2 = std::hash<TaskKey>{}(k2);
    // Different keys should (very likely) have different hashes
    assert(h != h2 || k.task_id == k2.task_id);

    printf("  PASS: TaskKey with full 7-level coordinate\n");
}

static void test_header_serialization_roundtrip() {
    LinquCoordinate sender;
    sender.l6_idx = 2;
    sender.l5_idx = 3;
    sender.l4_idx = 5;
    sender.l3_idx = 200;

    LinquCoordinate target;
    target.l6_idx = 1;
    target.l5_idx = 7;
    target.l4_idx = 12;
    target.l3_idx = 300;

    LinquHeader hdr = LinquHeader::make(MsgType::CALL_TASK, 6, sender, target, 1024);

    uint8_t wire[LinquHeader::WIRE_SIZE];
    hdr.serialize(wire);

    LinquHeader hdr2 = LinquHeader::deserialize(wire);
    assert(hdr2.magic == LINQU_MAGIC);
    assert(hdr2.msg_type == MsgType::CALL_TASK);
    assert(hdr2.sender_level == 6);
    assert(hdr2.payload_size == 1024);

    auto s = hdr2.get_sender();
    assert(s.l6_idx == 2);
    assert(s.l5_idx == 3);
    assert(s.l4_idx == 5);
    assert(s.l3_idx == 200);

    auto t = hdr2.get_target();
    assert(t.l6_idx == 1);
    assert(t.l5_idx == 7);
    assert(t.l4_idx == 12);
    assert(t.l3_idx == 300);

    printf("  PASS: LinquHeader serialize/deserialize with L4-L6 non-zero\n");
}

static void test_level_enum_coverage() {
    assert(level_value(Level::CORE) == 0);
    assert(level_value(Level::CHIP_DIE) == 1);
    assert(level_value(Level::CHIP) == 2);
    assert(level_value(Level::HOST) == 3);
    assert(level_value(Level::POD) == 4);
    assert(level_value(Level::CLOS1) == 5);
    assert(level_value(Level::CLOS2) == 6);

    printf("  PASS: Level enum all 7 values\n");
}

static void test_octet_mapper() {
    OctetMapper mapper;
    auto c = mapper.map("10.3.5.100");
    assert(c.l5_idx == 3);
    assert(c.l4_idx == 5);
    assert(c.l3_idx == 100);

    printf("  PASS: OctetMapper IP-to-coordinate\n");
}

static void test_fixed_mapper() {
    LinquCoordinate fixed;
    fixed.l6_idx = 1;
    fixed.l5_idx = 2;
    fixed.l4_idx = 3;
    fixed.l3_idx = 4;
    FixedMapper mapper(fixed);
    auto c = mapper.map("anything");
    assert(c.l6_idx == 1);
    assert(c.l5_idx == 2);
    assert(c.l4_idx == 3);
    assert(c.l3_idx == 4);

    printf("  PASS: FixedMapper returns fixed coordinate\n");
}

static void test_node_identity() {
    NodeIdentity id;
    id.coord.l6_idx = 1;
    id.coord.l5_idx = 2;
    id.physical_system_name = "rack-01";
    id.logical_system_name = "training-v2";

    std::string s = id.to_string();
    assert(s.find("rack-01") != std::string::npos);
    assert(s.find("training-v2") != std::string::npos);

    printf("  PASS: NodeIdentity with system names\n");
}

int main() {
    printf("=== Forward Compatibility: 7-Level Smoke Test ===\n");
    test_coordinate_full_7_levels();
    test_task_key_full_coordinate();
    test_header_serialization_roundtrip();
    test_level_enum_coverage();
    test_octet_mapper();
    test_fixed_mapper();
    test_node_identity();
    printf("ALL PASSED\n");
    return 0;
}
