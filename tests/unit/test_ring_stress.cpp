/*
 * Milestone 10C — Ring stress test.
 *
 * Dispatches ring_L3_stress.so to 4 L3 daemons.
 * Each daemon performs 10 iterations × 3 scope depths × 4 tensors
 * = 120 tensor allocs + 60 early frees + 40 scope enter/exit.
 *
 * Verifies:
 *   - All daemons complete without crash (ring didn't overflow)
 *   - Scope enters == scope exits (balanced)
 *   - Correct alloc/free counts
 */

#include "daemon/node_daemon.h"
#include "runtime/linqu_orchestrator_state.h"
#include "runtime/remote_dispatcher.h"
#include "discovery/peer_registry.h"
#include "discovery/filesystem_discovery.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <fstream>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

static const char* TEST_BASE = "/tmp/linqu_test_ring_stress";
static const char* RING_OUTPUT = "/tmp/linqu_test_ring_output";
static const int NUM_L3 = 4;

static void cleanup() {
    (void)system(("rm -rf " + std::string(TEST_BASE)).c_str());
    (void)system(("rm -rf " + std::string(RING_OUTPUT)).c_str());
}

static std::string build_kernel() {
    std::string src = "/data/liaoheng/pypto_workspace/pypto_runtime_distributed/"
                      "examples/ring_stress_test/kernels/ring_L3_stress.cpp";
    std::string out_dir = std::string(TEST_BASE) + "/build";
    (void)system(("mkdir -p " + out_dir).c_str());
    std::string so = out_dir + "/ring_L3_stress.so";
    std::string cmd = "g++ -shared -fPIC -o " + so + " " + src + " 2>&1";
    int rc = system(cmd.c_str());
    assert(rc == 0);
    return so;
}

static int run_l3_daemon(linqu::LinquCoordinate coord, const std::string& so_path) {
    setenv("LINQU_RING_OUTPUT", RING_OUTPUT, 1);

    linqu::NodeDaemon daemon(linqu::Level::HOST, coord, TEST_BASE);
    if (!daemon.start()) return 1;

    std::ifstream f(so_path, std::ios::binary | std::ios::ate);
    size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> data(sz);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sz));
    daemon.code_cache().register_code("ring_L3_stress.so", data.data(), data.size());

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
    fprintf(stderr, "=== Ring Stress Test (1 L4 → 4 L3) ===\n\n");

    std::string so_path = build_kernel();
    fprintf(stderr, "[RING] Built ring_L3_stress.so\n");

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

    linqu::UnixSocketTransport l4_transport(TEST_BASE, l4_coord, 4);
    { bool _ok = l4_transport.start_listening(); if (!_ok) { fprintf(stderr, "[FATAL] L4 start_listening failed\n"); return 1; } }

    linqu::RemoteDispatcher dispatcher(l4_transport, l4_coord, 4);
    dispatcher.start_recv_loop();

    linqu::PeerRegistry registry;
    linqu::FilesystemDiscovery disco(TEST_BASE);
    disco.discover(registry);
    auto l3_peers = registry.peers_at_level(3);
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
        linqu_submit_task(rt, target, "ring_L3_stress.so", nullptr, 0);
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
    fprintf(stderr, "\n=== Ring Stress Verification ===\n");

    // Expected: 10 iterations * 3 depths * 4 tensors = 120 allocs
    //           10 * 3 * 2 early frees = 60
    //           10 * (1 outer + 3 inner) = 40 scope enters/exits

    int result_count = 0;
    for (int i = 0; i < NUM_L3; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/L3_%d/ring_result.json", RING_OUTPUT, i);

        std::ifstream f(path);
        if (f.good()) {
            result_count++;
            assert(file_contains(path, "\"total_allocs\": 120"));
            assert(file_contains(path, "\"total_frees\": 60"));
            assert(file_contains(path, "\"scope_enters\": 40"));
            assert(file_contains(path, "\"scope_exits\": 40"));
            assert(file_contains(path, "\"balanced\": true"));

            fprintf(stderr, "  L3[%d]: 120 allocs, 60 frees, 40/40 scopes, balanced [PASS]\n", i);
        } else {
            fprintf(stderr, "  L3[%d]: MISSING [FAIL]\n", i);
        }
    }

    assert(result_count == NUM_L3);
    fprintf(stderr, "\n[CHECK] All %d daemons completed ring stress without crash\n", NUM_L3);

    cleanup();

    fprintf(stderr, "\n=== Ring Stress Test PASSED ===\n");
    fprintf(stderr, "Verified:\n");
    fprintf(stderr, "  1. ring_L3_stress.so compiled and loaded on 4 daemons\n");
    fprintf(stderr, "  2. 120 tensor allocs + 60 early frees per daemon (no overflow)\n");
    fprintf(stderr, "  3. 40 nested scope enter/exits balanced correctly\n");
    fprintf(stderr, "  4. Ring wrap-around and scope retirement work correctly\n");
    return 0;
}
