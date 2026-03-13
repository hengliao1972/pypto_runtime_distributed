#include "core/coordinate.h"
#include "core/task_key.h"
#include "transport/linqu_header.h"
#include "transport/msg_types.h"
#include "discovery/peer_registry.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    using namespace linqu;

    printf("=== test_forward_compat: 7-level coordinate smoke test ===\n");

    // Test 1: LinquCoordinate with all levels populated
    {
        LinquCoordinate c;
        c.l6_idx = 2; c.l5_idx = 15; c.l4_idx = 3;
        c.l3_idx = 11; c.l2_idx = 7; c.l1_idx = 1; c.l0_idx = 255;

        auto s = c.to_string();
        assert(s.find("l6=2") != std::string::npos);
        assert(s.find("l5=15") != std::string::npos);
        assert(s.find("l3=11") != std::string::npos);
        assert(s.find("l0=255") != std::string::npos);

        auto p = c.to_path();
        assert(p.find("L6_2") != std::string::npos);
        assert(p.find("L5_15") != std::string::npos);
        assert(p.find("L0_255") != std::string::npos);
        printf("  [PASS] Test 1: full 7-level coordinate\n");
    }

    // Test 2: TaskKey with non-zero upper levels
    {
        TaskKey k;
        k.logical_system = "system42";
        k.coord.l6_idx = 1;
        k.coord.l5_idx = 8;
        k.coord.l4_idx = 2;
        k.coord.l3_idx = 5;
        k.scope_depth = 3;
        k.task_id = 99;
        assert(k.coord.l6_idx == 1);
        assert(k.scope_depth == 3);

        auto s = k.to_string();
        assert(s.find("system42") != std::string::npos);
        assert(s.find("1,8,2,5") != std::string::npos);
        assert(s.find("d3") != std::string::npos);
        assert(s.find("t99") != std::string::npos);
        printf("  [PASS] Test 2: TaskKey with full hierarchy\n");
    }

    // Test 3: LinquHeader serialize/deserialize with all coordinate fields
    {
        LinquCoordinate sender;
        sender.l6_idx = 2; sender.l5_idx = 10; sender.l4_idx = 3; sender.l3_idx = 7;

        LinquCoordinate target;
        target.l6_idx = 1; target.l5_idx = 5; target.l4_idx = 1; target.l3_idx = 15;

        auto hdr = LinquHeader::make(MsgType::CALL_TASK, 6, sender, target, 128);

        uint8_t wire[LinquHeader::WIRE_SIZE];
        hdr.serialize(wire);
        auto decoded = LinquHeader::deserialize(wire);

        assert(decoded.magic == LINQU_MAGIC);
        assert(decoded.msg_type == MsgType::CALL_TASK);
        assert(decoded.sender_level == 6);
        assert(decoded.payload_size == 128);

        auto s = decoded.get_sender();
        assert(s.l6_idx == 2);
        assert(s.l5_idx == 10);
        assert(s.l4_idx == 3);

        auto t = decoded.get_target();
        assert(t.l6_idx == 1);
        assert(t.l5_idx == 5);
        assert(t.l4_idx == 1);
        assert(t.l3_idx == 15);
        printf("  [PASS] Test 3: header roundtrip with full coordinates\n");
    }

    // Test 4: PeerRegistry queries with full coordinates
    {
        PeerRegistry reg;

        PeerInfo p1;
        p1.coord.l6_idx = 0; p1.coord.l5_idx = 3; p1.coord.l4_idx = 1; p1.coord.l3_idx = 5;
        p1.level = 3; p1.socket_path = "/tmp/a.sock"; p1.alive = true;
        reg.add(p1);

        PeerInfo p2;
        p2.coord.l6_idx = 1; p2.coord.l5_idx = 7; p2.coord.l4_idx = 2; p2.coord.l3_idx = 10;
        p2.level = 3; p2.socket_path = "/tmp/b.sock"; p2.alive = true;
        reg.add(p2);

        PeerInfo p3;
        p3.coord.l6_idx = 0; p3.coord.l5_idx = 3; p3.coord.l4_idx = 1; p3.coord.l3_idx = 8;
        p3.level = 3; p3.socket_path = "/tmp/c.sock"; p3.alive = true;
        reg.add(p3);

        auto l3_peers = reg.peers_at_level(3);
        assert(l3_peers.size() == 3);

        auto same_pod = reg.peers_same_parent(p1.coord, 3);
        assert(same_pod.size() == 2);
        printf("  [PASS] Test 4: PeerRegistry with multi-level coordinates\n");
    }

    // Test 5: Coordinate comparison/ordering
    {
        LinquCoordinate a, b;
        a.l6_idx = 0; a.l5_idx = 5; a.l4_idx = 2; a.l3_idx = 10;
        b.l6_idx = 0; b.l5_idx = 5; b.l4_idx = 2; b.l3_idx = 11;
        assert(a < b);
        assert(!(b < a));
        assert(a != b);

        LinquCoordinate c = a;
        assert(a == c);
        printf("  [PASS] Test 5: coordinate comparison\n");
    }

    printf("=== all forward-compat tests passed ===\n");
    return 0;
}
