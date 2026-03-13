#include "discovery/peer_registry.h"
#include "core/coordinate.h"
#include <cassert>
#include <cstdio>

using namespace linqu;

static void test_7_level_peer_registry() {
    PeerRegistry registry;

    for (int l6 = 0; l6 < 2; l6++) {
        for (int l5 = 0; l5 < 4; l5++) {
            for (int l4 = 0; l4 < 4; l4++) {
                for (int l3 = 0; l3 < 4; l3++) {
                    PeerInfo info;
                    info.coord.l6_idx = l6;
                    info.coord.l5_idx = l5;
                    info.coord.l4_idx = l4;
                    info.coord.l3_idx = l3;
                    info.level = 3;
                    info.alive = true;
                    registry.add(info);
                }
            }
        }
    }

    // Total: 2 * 4 * 4 * 4 = 128 peers
    auto all = registry.all_peers();
    assert(all.size() == 128);

    // Query L3 peers
    auto l3_peers = registry.peers_at_level(3);
    assert(l3_peers.size() == 128);

    // No peers at L4 since we only registered at level 3
    auto l4_peers = registry.peers_at_level(4);
    assert(l4_peers.size() == 0);

    printf("  PASS: 7-level PeerRegistry with 128 peers\n");
}

static void test_peer_filter_by_parent() {
    PeerRegistry registry;

    // Add peers in supernode (l6=0, l5=1)
    for (int l4 = 0; l4 < 4; l4++) {
        for (int l3 = 0; l3 < 8; l3++) {
            PeerInfo info;
            info.coord.l6_idx = 0;
            info.coord.l5_idx = 1;
            info.coord.l4_idx = l4;
            info.coord.l3_idx = l3;
            info.level = 3;
            info.alive = true;
            registry.add(info);
        }
    }

    auto all = registry.all_peers();
    assert(all.size() == 32);

    auto l3_list = registry.peers_at_level(3);
    assert(l3_list.size() == 32);

    // Filter by parent: same L4 parent (l4=2)
    LinquCoordinate self;
    self.l6_idx = 0;
    self.l5_idx = 1;
    self.l4_idx = 2;
    self.l3_idx = 0;
    auto siblings = registry.peers_same_parent(self, 3);
    assert(siblings.size() == 8);

    printf("  PASS: peer filter by parent coordinate\n");
}

static void test_peer_alive_transitions() {
    PeerRegistry registry;

    PeerInfo p1;
    p1.coord.l6_idx = 0;
    p1.coord.l5_idx = 0;
    p1.coord.l4_idx = 0;
    p1.coord.l3_idx = 1;
    p1.level = 3;
    p1.alive = true;
    registry.add(p1);

    auto peers = registry.all_peers();
    assert(peers.size() == 1);
    assert(peers[0].alive == true);

    // Mark as dead
    registry.set_alive(p1.coord, 3, false);
    peers = registry.all_peers();
    assert(peers.size() == 1);
    assert(peers[0].alive == false);

    // Mark as alive again
    registry.set_alive(p1.coord, 3, true);
    peers = registry.all_peers();
    assert(peers.size() == 1);
    assert(peers[0].alive == true);

    printf("  PASS: peer alive transitions (live -> dead -> live)\n");
}

int main() {
    printf("=== Forward Compatibility: PeerRegistry Multi-Level ===\n");
    test_7_level_peer_registry();
    test_peer_filter_by_parent();
    test_peer_alive_transitions();
    printf("ALL PASSED\n");
    return 0;
}
