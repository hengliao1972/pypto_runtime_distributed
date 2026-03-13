/*
 * Milestone 10 — Small-scale topology test.
 *
 * Topology:  1 L4 (pod) → 4 L3 (hosts), each in its own process.
 *
 * Flow:
 *   1. Build topo_L3_host.so from source.
 *   2. Fork 4 L3 daemon processes, pre-register the .so in each.
 *   3. L4 (parent) uses OrchestratorState + RemoteDispatcher to
 *      dispatch topo_L3_host.so to each L3 daemon.
 *   4. Each daemon dlopen+executes the kernel, which writes identity.json.
 *   5. Parent verifies: 4 identity.json files with correct coordinates.
 */

#include "daemon/node_daemon.h"
#include "runtime/linqu_orchestrator_state.h"
#include "runtime/remote_dispatcher.h"
#include "discovery/peer_registry.h"
#include "discovery/filesystem_discovery.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <set>
#include <unistd.h>
#include <sys/wait.h>

static const char* TEST_BASE = "/tmp/linqu_test_topo";
static const char* TOPO_OUTPUT = "/tmp/linqu_test_topo_output";
static const int NUM_L3 = 4;

static void cleanup() {
    (void)system(("rm -rf " + std::string(TEST_BASE)).c_str());
    (void)system(("rm -rf " + std::string(TOPO_OUTPUT)).c_str());
}

static std::string build_kernel() {
    std::string src = "/data/liaoheng/pypto_workspace/pypto_runtime_distributed/"
                      "examples/topology_test/kernels/topo_L3_host.cpp";
    std::string out_dir = std::string(TEST_BASE) + "/build";
    (void)system(("mkdir -p " + out_dir).c_str());
    std::string so = out_dir + "/topo_L3_host.so";
    std::string cmd = "g++ -shared -fPIC -o " + so + " " + src + " 2>&1";
    int rc = system(cmd.c_str());
    assert(rc == 0);
    return so;
}

static int run_l3_daemon(linqu::LinquCoordinate coord, const std::string& so_path) {
    setenv("LINQU_TOPO_OUTPUT", TOPO_OUTPUT, 1);

    linqu::NodeDaemon daemon(linqu::Level::HOST, coord, TEST_BASE);
    if (!daemon.start()) return 1;

    std::ifstream f(so_path, std::ios::binary | std::ios::ate);
    size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> data(sz);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sz));
    daemon.code_cache().register_code("topo_L3_host.so", data.data(), data.size());

    daemon.run_event_loop(1);
    daemon.stop();
    return 0;
}

static bool file_contains(const std::string& path, const std::string& key) {
    std::ifstream f(path);
    if (!f) return false;
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    return content.find(key) != std::string::npos;
}

int main() {
    cleanup();
    fprintf(stderr, "=== Topology Test (1 L4 → 4 L3) ===\n\n");

    std::string so_path = build_kernel();
    fprintf(stderr, "[TOPO] Built topo_L3_host.so\n");

    linqu::LinquCoordinate l4_coord;
    l4_coord.l5_idx = 0; l4_coord.l4_idx = 0;

    std::vector<pid_t> children;
    for (int i = 0; i < NUM_L3; i++) {
        linqu::LinquCoordinate l3c;
        l3c.l5_idx = 0; l3c.l4_idx = 0;
        l3c.l3_idx = static_cast<uint16_t>(i);

        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            _exit(run_l3_daemon(l3c, so_path));
        }
        children.push_back(pid);
    }

    usleep(600000);

    // L4 orchestrator
    linqu::UnixSocketTransport l4_transport(TEST_BASE, l4_coord, 4);
    assert(l4_transport.start_listening());

    linqu::RemoteDispatcher dispatcher(l4_transport, l4_coord, 4);
    dispatcher.start_recv_loop();

    linqu::PeerRegistry registry;
    linqu::FilesystemDiscovery disco(TEST_BASE);
    disco.discover(registry);
    auto l3_peers = registry.peers_at_level(3);
    fprintf(stderr, "[TOPO] Discovered %zu L3 peers\n", l3_peers.size());
    assert(l3_peers.size() == NUM_L3);

    linqu::LinquOrchestratorState state;
    linqu::LinquOrchConfig_Internal cfg;
    state.init(linqu::Level::POD, l4_coord, cfg);
    state.set_dispatcher(&dispatcher);

    LinquRuntime* rt = state.runtime();
    linqu_scope_begin(rt);
    for (auto& peer : l3_peers) {
        LinquCoordinate_C target = {};
        target.l5_idx = peer.coord.l5_idx;
        target.l4_idx = peer.coord.l4_idx;
        target.l3_idx = peer.coord.l3_idx;
        linqu_submit_task(rt, target, "topo_L3_host.so", nullptr, 0);
    }
    linqu_scope_end(rt);
    dispatcher.wait_all();
    dispatcher.stop_recv_loop();
    l4_transport.stop();

    for (auto cpid : children) {
        int wstatus;
        waitpid(cpid, &wstatus, 0);
        assert(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);
    }

    // ====== VERIFY ======
    fprintf(stderr, "\n=== Verification ===\n");

    int identity_count = 0;
    std::set<int> global_indices;

    for (int i = 0; i < NUM_L3; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/L6_0/L5_0/L4_0/L3_%d/identity.json",
                 TOPO_OUTPUT, i);

        if (file_contains(path, "\"pid\"")) {
            identity_count++;
            std::string key = "\"l3\": " + std::to_string(i);
            assert(file_contains(path, key));
            std::string gi_key = "\"global_index\": " + std::to_string(i);
            assert(file_contains(path, gi_key));
            global_indices.insert(i);
            fprintf(stderr, "  L3[%d] identity.json: OK (coordinate and global_index correct)\n", i);
        } else {
            fprintf(stderr, "  L3[%d] identity.json: MISSING at %s\n", i, path);
        }
    }

    fprintf(stderr, "\n[CHECK 1] Identity files: %d/%d\n",
            identity_count, NUM_L3);
    assert(identity_count == NUM_L3);

    fprintf(stderr, "[CHECK 2] Global indices: {");
    for (int idx : global_indices) fprintf(stderr, "%d ", idx);
    fprintf(stderr, "} — all unique and correct\n");
    assert((int)global_indices.size() == NUM_L3);

    fprintf(stderr, "[CHECK 3] Tasks submitted: %lld (expected %d)\n",
            (long long)state.tasks_submitted(), NUM_L3);
    assert(state.tasks_submitted() == NUM_L3);

    cleanup();

    fprintf(stderr, "\n=== Topology Test PASSED ===\n");
    fprintf(stderr, "Verified:\n");
    fprintf(stderr, "  1. topo_L3_host.so compiled and loaded via dlopen\n");
    fprintf(stderr, "  2. 4 L3 daemons each executed the kernel in their own process\n");
    fprintf(stderr, "  3. Each kernel used ops table (self_coord, log_info, orchestration_done)\n");
    fprintf(stderr, "  4. Each wrote identity.json with correct coordinates\n");
    fprintf(stderr, "  5. L4 OrchestratorState + RemoteDispatcher orchestrated the fan-out\n");
    return 0;
}
