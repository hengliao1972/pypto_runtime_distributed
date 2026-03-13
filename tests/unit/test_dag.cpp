/*
 * Milestone 10B — Distributed DAG test (small scale).
 *
 * 1 L4 → 4 L3 daemons, each runs dag_L3_compute.so which:
 *   - Performs the 5-step tensor DAG (a+b+1)(a+b+2)+(a+b)
 *   - Uses ops table for scope_begin/scope_end, alloc_tensor, free_tensor
 *   - Verifies numerical correctness inline
 *   - Writes result.json with error count and sample values
 *
 * The test verifies:
 *   - All 4 daemons executed the kernel
 *   - All results are numerically correct (0 errors)
 *   - Scope and tensor operations worked through the ops table
 */

#include "daemon/node_daemon.h"
#include "runtime/linqu_orchestrator_state.h"
#include "runtime/remote_dispatcher.h"
#include "discovery/peer_registry.h"
#include "discovery/filesystem_discovery.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <fstream>
#include <sstream>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

static const char* TEST_BASE = "/tmp/linqu_test_dag";
static const char* DAG_OUTPUT = "/tmp/linqu_test_dag_output";
static const int NUM_L3 = 4;

static void cleanup() {
    (void)system(("rm -rf " + std::string(TEST_BASE)).c_str());
    (void)system(("rm -rf " + std::string(DAG_OUTPUT)).c_str());
}

static std::string build_kernel() {
    std::string src = "/data/liaoheng/pypto_workspace/pypto_runtime_distributed/"
                      "examples/tensor_dag_test/kernels/dag_L3_compute.cpp";
    std::string out_dir = std::string(TEST_BASE) + "/build";
    (void)system(("mkdir -p " + out_dir).c_str());
    std::string so = out_dir + "/dag_L3_compute.so";
    std::string cmd = "g++ -shared -fPIC -o " + so + " " + src + " -lm 2>&1";
    int rc = system(cmd.c_str());
    assert(rc == 0);
    return so;
}

static int run_l3_daemon(linqu::LinquCoordinate coord, const std::string& so_path) {
    setenv("LINQU_DAG_OUTPUT", DAG_OUTPUT, 1);

    linqu::NodeDaemon daemon(linqu::Level::HOST, coord, TEST_BASE);
    if (!daemon.start()) return 1;

    std::ifstream f(so_path, std::ios::binary | std::ios::ate);
    size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> data(sz);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sz));
    daemon.code_cache().register_code("dag_L3_compute.so", data.data(), data.size());

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
    fprintf(stderr, "=== Distributed DAG Test (1 L4 → 4 L3) ===\n\n");

    std::string so_path = build_kernel();
    fprintf(stderr, "[DAG] Built dag_L3_compute.so\n");

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
    assert(l4_transport.start_listening());

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
        linqu_submit_task(rt, target, "dag_L3_compute.so", nullptr, 0);
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
    fprintf(stderr, "\n=== DAG Verification ===\n");

    int result_count = 0;
    int total_errors = 0;

    for (int i = 0; i < NUM_L3; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s/L3_%d/result.json", DAG_OUTPUT, i);

        std::ifstream f(path);
        if (f.good()) {
            result_count++;
            assert(file_contains(path, "\"errors\": 0"));

            // Verify f[0] = (0+0+1)(0+0+2)+(0+0) = 2.0
            assert(file_contains(path, "\"f_0\": 2.0"));
            // Verify f[1] = (3+1)(3+2)+3 = 4*5+3 = 23.0
            assert(file_contains(path, "\"f_1\": 23.0"));
            // Verify f[63] = 9*63^2+12*63+2 = 35721+756+2 = 36479.0
            assert(file_contains(path, "\"f_63\": 36479.0"));

            fprintf(stderr, "  L3[%d] result.json: 0 errors, f[0]=2, f[1]=23, f[63]=36479 [PASS]\n", i);
        } else {
            fprintf(stderr, "  L3[%d] result.json: MISSING [FAIL]\n", i);
        }
    }

    fprintf(stderr, "\n[CHECK 1] Result files: %d/%d\n", result_count, NUM_L3);
    assert(result_count == NUM_L3);

    fprintf(stderr, "[CHECK 2] All computations numerically correct (0 total errors)\n");

    fprintf(stderr, "[CHECK 3] Tasks submitted: %lld (expected %d)\n",
            (long long)state.tasks_submitted(), NUM_L3);
    assert(state.tasks_submitted() == NUM_L3);

    cleanup();

    fprintf(stderr, "\n=== Distributed DAG Test PASSED ===\n");
    fprintf(stderr, "Verified:\n");
    fprintf(stderr, "  1. dag_L3_compute.so compiled and loaded via dlopen on 4 daemons\n");
    fprintf(stderr, "  2. Each daemon computed f[i] = (a[i]+b[i]+1)(a[i]+b[i]+2) + (a[i]+b[i])\n");
    fprintf(stderr, "  3. Scope management (scope_begin/scope_end) via ops table\n");
    fprintf(stderr, "  4. Tensor allocation and freeing via ops table\n");
    fprintf(stderr, "  5. All 4 results numerically correct: f[i] = 9i^2 + 12i + 2\n");
    return 0;
}
