#include "runtime/linqu_orchestrator_state.h"
#include "discovery/peer_registry.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace linqu;

static void test_query_peers_empty() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 8;
    cfg.heap_ring_capacity = 8192;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    LinquRuntime* rt = state.runtime();
    LinquPeerList pl = linqu_query_peers(rt, 3);
    assert(pl.count == 0);
    assert(pl.peers == nullptr);

    printf("  query_peers_empty: PASS\n");
}

static void test_query_peers_with_registry() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 8;
    cfg.heap_ring_capacity = 8192;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    PeerRegistry registry;

    PeerInfo p1;
    p1.coord.l3_idx = 0;
    p1.level = 3;
    p1.socket_path = "/tmp/test_L3_0.sock";
    registry.add(p1);

    PeerInfo p2;
    p2.coord.l3_idx = 1;
    p2.level = 3;
    p2.socket_path = "/tmp/test_L3_1.sock";
    registry.add(p2);

    PeerInfo p3;
    p3.coord.l4_idx = 1;
    p3.level = 4;
    p3.socket_path = "/tmp/test_L4_1.sock";
    registry.add(p3);

    state.set_peer_registry(&registry);

    LinquRuntime* rt = state.runtime();

    LinquPeerList pl3 = linqu_query_peers(rt, 3);
    assert(pl3.count == 2);
    assert(pl3.peers != nullptr);

    bool found_0 = false, found_1 = false;
    for (int i = 0; i < pl3.count; i++) {
        if (pl3.peers[i].l3_idx == 0) found_0 = true;
        if (pl3.peers[i].l3_idx == 1) found_1 = true;
    }
    assert(found_0 && found_1);
    free(pl3.peers);

    LinquPeerList pl4 = linqu_query_peers(rt, 4);
    assert(pl4.count == 1);
    assert(pl4.peers[0].l4_idx == 1);
    free(pl4.peers);

    LinquPeerList pl5 = linqu_query_peers(rt, 5);
    assert(pl5.count == 0);

    printf("  query_peers_with_registry: PASS\n");
}

static void test_query_peers_from_kernel() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 8;
    cfg.heap_ring_capacity = 8192;
    state.init(Level::CLOS1, LinquCoordinate{}, cfg);

    PeerRegistry registry;

    for (int i = 0; i < 4; i++) {
        PeerInfo p;
        p.coord.l4_idx = static_cast<uint8_t>(i);
        p.level = 4;
        p.socket_path = "/tmp/pod_" + std::to_string(i) + ".sock";
        registry.add(p);
    }

    state.set_peer_registry(&registry);

    LinquRuntime* rt = state.runtime();

    LinquPeerList pl = rt->ops->query_peers(rt, 4);
    assert(pl.count == 4);

    for (int i = 0; i < pl.count; i++) {
        assert(pl.peers[i].l4_idx < 4);
    }
    free(pl.peers);

    printf("  query_peers_from_kernel: PASS\n");
}

int main() {
    printf("=== test_query_peers ===\n");
    test_query_peers_empty();
    test_query_peers_with_registry();
    test_query_peers_from_kernel();
    printf("=== ALL PASS ===\n");
    return 0;
}
