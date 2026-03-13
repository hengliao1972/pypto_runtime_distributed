#include "discovery/peer_registry.h"
#include "discovery/filesystem_discovery.h"
#include "transport/unix_socket_transport.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <unistd.h>

static const char* TEST_BASE = "/tmp/linqu_test_discovery";

static void cleanup() {
    (void)system(("rm -rf " + std::string(TEST_BASE)).c_str());
}

// ================================================================
// Test 1: PeerRegistry basic CRUD
// ================================================================
static void test_peer_registry_basic() {
    fprintf(stderr, "--- Test: PeerRegistry basic ---\n");

    linqu::PeerRegistry reg;
    assert(reg.count() == 0);

    linqu::PeerInfo p1;
    p1.coord.l5_idx = 0; p1.coord.l4_idx = 0; p1.coord.l3_idx = 0;
    p1.level = 3;
    p1.socket_path = "/tmp/test/L3_0/daemon_L3.sock";
    reg.add(p1);

    linqu::PeerInfo p2;
    p2.coord.l5_idx = 0; p2.coord.l4_idx = 0; p2.coord.l3_idx = 1;
    p2.level = 3;
    p2.socket_path = "/tmp/test/L3_1/daemon_L3.sock";
    reg.add(p2);

    linqu::PeerInfo p3;
    p3.coord.l5_idx = 0; p3.coord.l4_idx = 1; p3.coord.l3_idx = 0;
    p3.level = 3;
    p3.socket_path = "/tmp/test/L4_1/L3_0/daemon_L3.sock";
    reg.add(p3);

    linqu::PeerInfo p4;
    p4.coord.l5_idx = 0; p4.coord.l4_idx = 0;
    p4.level = 4;
    p4.socket_path = "/tmp/test/L4_0/daemon_L4.sock";
    reg.add(p4);

    assert(reg.count() == 4);

    auto l3_peers = reg.peers_at_level(3);
    assert(l3_peers.size() == 3);

    auto l4_peers = reg.peers_at_level(4);
    assert(l4_peers.size() == 1);

    // peers_same_parent: L3 peers in same L4 as p1 (l4=0)
    linqu::LinquCoordinate self;
    self.l5_idx = 0; self.l4_idx = 0;
    auto same_pod = reg.peers_same_parent(self, 3);
    assert(same_pod.size() == 2); // p1 and p2

    // Mark p2 dead
    reg.set_alive(p2.coord, 3, false);
    same_pod = reg.peers_same_parent(self, 3);
    assert(same_pod.size() == 1);

    reg.remove(p1.coord, 3);
    assert(reg.count() == 3);

    reg.clear();
    assert(reg.count() == 0);

    fprintf(stderr, "  PeerRegistry CRUD: add, query, set_alive, remove, clear [PASS]\n");
}

// ================================================================
// Test 2: FilesystemDiscovery scans socket files
// ================================================================
static void test_filesystem_discovery() {
    fprintf(stderr, "--- Test: FilesystemDiscovery ---\n");
    cleanup();

    // Create a mini topology: 1 L4 + 4 L3 + 1 L5 socket files
    std::vector<linqu::UnixSocketTransport*> transports;

    auto make_transport = [&](linqu::LinquCoordinate coord, uint8_t level)
        -> linqu::UnixSocketTransport* {
        auto* t = new linqu::UnixSocketTransport(TEST_BASE, coord, level);
        assert(t->start_listening());
        transports.push_back(t);
        return t;
    };

    linqu::LinquCoordinate l5c; l5c.l5_idx = 2;
    make_transport(l5c, 5);

    linqu::LinquCoordinate l4c; l4c.l5_idx = 2; l4c.l4_idx = 1;
    make_transport(l4c, 4);

    for (int i = 0; i < 4; i++) {
        linqu::LinquCoordinate l3c;
        l3c.l5_idx = 2; l3c.l4_idx = 1;
        l3c.l3_idx = static_cast<uint16_t>(i);
        make_transport(l3c, 3);
    }

    // Discovery
    linqu::PeerRegistry registry;
    linqu::FilesystemDiscovery disco(TEST_BASE);
    int found = disco.discover(registry);
    fprintf(stderr, "  Discovered %d socket files\n", found);
    assert(found == 6);
    assert(registry.count() == 6);

    auto l3_peers = registry.peers_at_level(3);
    assert(l3_peers.size() == 4);

    auto l4_peers = registry.peers_at_level(4);
    assert(l4_peers.size() == 1);
    assert(l4_peers[0].coord.l4_idx == 1);

    auto l5_peers = registry.peers_at_level(5);
    assert(l5_peers.size() == 1);
    assert(l5_peers[0].coord.l5_idx == 2);

    // Verify same-parent query: L3 peers under l4=1
    linqu::LinquCoordinate self;
    self.l5_idx = 2; self.l4_idx = 1;
    auto same_pod = registry.peers_same_parent(self, 3);
    assert(same_pod.size() == 4);

    // Verify coordinate parsing
    std::set<int> l3_indices;
    for (auto& p : same_pod) {
        l3_indices.insert(p.coord.l3_idx);
    }
    assert(l3_indices.size() == 4);
    assert(l3_indices.count(0) && l3_indices.count(1) &&
           l3_indices.count(2) && l3_indices.count(3));

    for (auto* t : transports) {
        t->stop();
        delete t;
    }
    cleanup();

    fprintf(stderr, "  FilesystemDiscovery: found 6 sockets (1 L5, 1 L4, 4 L3), "
            "coordinates parsed correctly [PASS]\n");
}

// ================================================================
// Test 3: Multi-pod discovery with peers_same_parent filtering
// ================================================================
static void test_multi_pod_discovery() {
    fprintf(stderr, "--- Test: Multi-pod discovery ---\n");
    cleanup();

    std::vector<linqu::UnixSocketTransport*> transports;

    // Create 2 pods × 3 L3 nodes = 6 L3 nodes + 2 L4 nodes
    for (int pod = 0; pod < 2; pod++) {
        linqu::LinquCoordinate l4c;
        l4c.l5_idx = 0; l4c.l4_idx = static_cast<uint8_t>(pod);
        auto* t = new linqu::UnixSocketTransport(TEST_BASE, l4c, 4);
        assert(t->start_listening());
        transports.push_back(t);

        for (int h = 0; h < 3; h++) {
            linqu::LinquCoordinate l3c;
            l3c.l5_idx = 0;
            l3c.l4_idx = static_cast<uint8_t>(pod);
            l3c.l3_idx = static_cast<uint16_t>(h);
            auto* t2 = new linqu::UnixSocketTransport(TEST_BASE, l3c, 3);
            assert(t2->start_listening());
            transports.push_back(t2);
        }
    }

    linqu::PeerRegistry registry;
    linqu::FilesystemDiscovery disco(TEST_BASE);
    int found = disco.discover(registry);
    assert(found == 8); // 2 L4 + 6 L3

    // From pod 0's perspective: only 3 L3 peers with l4=0
    linqu::LinquCoordinate self_pod0;
    self_pod0.l5_idx = 0; self_pod0.l4_idx = 0;
    auto pod0_hosts = registry.peers_same_parent(self_pod0, 3);
    assert(pod0_hosts.size() == 3);
    for (auto& p : pod0_hosts) assert(p.coord.l4_idx == 0);

    // From pod 1's perspective: only 3 L3 peers with l4=1
    linqu::LinquCoordinate self_pod1;
    self_pod1.l5_idx = 0; self_pod1.l4_idx = 1;
    auto pod1_hosts = registry.peers_same_parent(self_pod1, 3);
    assert(pod1_hosts.size() == 3);
    for (auto& p : pod1_hosts) assert(p.coord.l4_idx == 1);

    // All L3 peers across pods
    auto all_l3 = registry.peers_at_level(3);
    assert(all_l3.size() == 6);

    // L4 peers under same L5
    auto same_sn_pods = registry.peers_same_parent(self_pod0, 4);
    assert(same_sn_pods.size() == 2);

    for (auto* t : transports) { t->stop(); delete t; }
    cleanup();

    fprintf(stderr, "  Multi-pod: 2 pods × 3 hosts, correct same-parent filtering [PASS]\n");
}

int main() {
    fprintf(stderr, "=== Discovery Tests ===\n\n");
    test_peer_registry_basic();
    test_filesystem_discovery();
    test_multi_pod_discovery();
    fprintf(stderr, "\n=== All Discovery Tests PASSED ===\n");
    return 0;
}
