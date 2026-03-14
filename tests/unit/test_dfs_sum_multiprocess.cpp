/*
 * test_dfs_sum_multiprocess — DFS hierarchical sum test using real
 * multi-process .so kernel dispatch (L4 → N L3 daemons).
 *
 * Topology:  1 L4 orchestrator → L4_NUM_L3 L3 daemons (default 16)
 *
 * Flow:
 *  1. Generate L4_NUM_L3 lingqu_dfs data files (simulated shared namespace).
 *     Each file contains NUMS_PER_FILE integers.
 *  2. Compile dfs_L3_reader.so from source.
 *  3. Fork L4_NUM_L3 L3 NodeDaemon processes; register the .so in each.
 *  4. L4 orchestrator (parent) uses LinquOrchestratorState + RemoteDispatcher
 *     to dispatch dfs_L3_reader.so to all L3 daemons.
 *  5. Each L3 daemon executes dfs_L3_reader, reads its DFS data file,
 *     writes local_sum to sum.json.
 *  6. Parent reads all sum.json files, computes total sum.
 *  7. Assert computed total == expected total (cross-verified with in-process
 *     generation).
 *
 * This test demonstrates:
 *  - FunctionRole extension: dfs_L3_reader reports role=WORKER via
 *    linqu_orch_config().role == 1 (LINQU_ROLE_WORKER).
 *  - lingqu_dfs shared-namespace file I/O accessed from L3 (HOST) level.
 *  - Real multi-process .so kernel chain with file-based result communication.
 */

#include "daemon/node_daemon.h"
#include "runtime/linqu_orchestrator_state.h"
#include "runtime/remote_dispatcher.h"
#include "runtime/linqu_orchestration_api.h"
#include "discovery/peer_registry.h"
#include "discovery/filesystem_discovery.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

static const char* TEST_BASE   = "/tmp/linqu_test_dfs_mp";
static const char* DFS_BASE    = "/tmp/linqu_test_dfs_mp/dfs";
static const char* DFS_OUTPUT  = "/tmp/linqu_test_dfs_mp/dfs_output";

static const int   NUMS_PER_FILE = 1024;

/* Number of L3 nodes in this test (controllable via env). */
static int NUM_L3 = 16;

/* ------------------------------------------------------------------ */

static void cleanup() {
    (void)system(("rm -rf " + std::string(TEST_BASE)).c_str());
}

static void mkdir_p(const std::string& path) {
    (void)system(("mkdir -p " + path).c_str());
}

/* Generate DFS data files; return expected total sum. */
static uint64_t build_dfs_files(int num_l3) {
    mkdir_p(std::string(DFS_BASE));
    uint64_t total = 0;
    for (int i = 0; i < num_l3; i++) {
        std::string dir = std::string(DFS_BASE) + "/L3_" + std::to_string(i);
        mkdir_p(dir);
        std::string path = dir + "/data.txt";

        std::mt19937 rng(0xDEAD0000u + static_cast<uint32_t>(i));
        std::uniform_int_distribution<int> dist(1, 1000);

        std::ofstream out(path);
        assert(out.good());
        for (int k = 0; k < NUMS_PER_FILE; k++) {
            int v = dist(rng);
            total += static_cast<uint64_t>(v);
            out << v << "\n";
        }
    }
    return total;
}

/* Compile dfs_L3_reader.so and return its path. */
static std::string build_kernel() {
    std::string src =
        "/data/liaoheng/pypto_workspace/pypto_runtime_distributed/"
        "examples/dfs_sum_test/kernels/dfs_L3_reader.cpp";
    std::string out_dir = std::string(TEST_BASE) + "/build";
    mkdir_p(out_dir);
    std::string so = out_dir + "/dfs_L3_reader.so";
    std::string cmd = "g++ -shared -fPIC -o " + so + " " + src + " 2>&1";
    int rc = system(cmd.c_str());
    if (rc != 0) {
        fprintf(stderr, "[BUILD] g++ failed for dfs_L3_reader.so\n");
    }
    assert(rc == 0);
    return so;
}

/* Run a single L3 NodeDaemon in a child process. */
static int run_l3_daemon(linqu::LinquCoordinate coord,
                          const std::string& so_path,
                          int global_l3) {
    setenv("LINQU_DFS_BASE",   DFS_BASE,   1);
    setenv("LINQU_DFS_OUTPUT", DFS_OUTPUT, 1);

    /* Encode global_l3 index into l3_idx for self_coord lookup inside kernel.
     * For a flat L4→L3 topology l6=0, l5=0, l4=0, l3=global_l3. */
    linqu::NodeDaemon daemon(linqu::Level::HOST, coord, TEST_BASE);
    if (!daemon.start()) return 1;

    std::ifstream f(so_path, std::ios::binary | std::ios::ate);
    size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> data(sz);
    f.read(reinterpret_cast<char*>(data.data()),
           static_cast<std::streamsize>(sz));
    daemon.code_cache().register_code("dfs_L3_reader.so", data.data(), data.size());

    daemon.run_event_loop(1);
    daemon.stop();
    (void)global_l3;
    return 0;
}

/* Parse uint64_t from "\"local_sum\": <value>" in sum.json */
static uint64_t read_local_sum(const std::string& path) {
    std::ifstream f(path);
    if (!f.good()) return 0;
    std::string line;
    while (std::getline(f, line)) {
        if (line.find("local_sum") != std::string::npos) {
            auto pos = line.rfind(':');
            if (pos != std::string::npos) {
                return std::stoull(line.substr(pos + 1));
            }
        }
    }
    return 0;
}

/* Verify role field exported by the compiled .so. */
static void verify_kernel_role(const std::string& so_path) {
    void* handle = dlopen(so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    assert(handle && "dlopen dfs_L3_reader.so failed");

    using ConfigFn = LinquOrchConfig(*)(uint64_t*, int);
    auto cfg_fn = reinterpret_cast<ConfigFn>(dlsym(handle, "linqu_orch_config"));
    assert(cfg_fn && "dlsym linqu_orch_config failed");

    LinquOrchConfig cfg = cfg_fn(nullptr, 0);
    assert(cfg.level == 3);
    assert(cfg.role  == LINQU_ROLE_WORKER &&
           "dfs_L3_reader must report role = LINQU_ROLE_WORKER");
    fprintf(stderr, "[ROLE] dfs_L3_reader.so: level=%d role=%s [PASS]\n",
            cfg.level,
            cfg.role == LINQU_ROLE_WORKER ? "WORKER" : "ORCHESTRATOR");

    dlclose(handle);
}

/* ------------------------------------------------------------------ */

int main() {
    cleanup();

    const char* n_env = getenv("DFS_NUM_L3");
    if (n_env && atoi(n_env) > 0) NUM_L3 = atoi(n_env);

    fprintf(stderr,
            "=== DFS Sum Multiprocess Test (1 L4 → %d L3 daemons) ===\n\n",
            NUM_L3);

    /* 1. Build DFS files */
    const uint64_t expected_total = build_dfs_files(NUM_L3);
    fprintf(stderr, "[DFS] Generated %d data files, expected total=%llu\n",
            NUM_L3, (unsigned long long)expected_total);

    /* 2. Compile dfs_L3_reader.so */
    std::string so_path = build_kernel();
    fprintf(stderr, "[BUILD] dfs_L3_reader.so ready\n");

    /* 3. Verify role field */
    verify_kernel_role(so_path);

    /* 4. Fork L3 daemons */
    linqu::LinquCoordinate l4_coord;
    l4_coord.l5_idx = 0;
    l4_coord.l4_idx = 0;

    std::vector<pid_t> children;
    children.reserve(static_cast<size_t>(NUM_L3));
    for (int i = 0; i < NUM_L3; i++) {
        linqu::LinquCoordinate l3c;
        l3c.l5_idx = 0;
        l3c.l4_idx = 0;
        l3c.l3_idx = static_cast<uint16_t>(i);

        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            _exit(run_l3_daemon(l3c, so_path, i));
        }
        children.push_back(pid);
    }

    usleep(600000);  /* wait for daemons to bind sockets */

    /* 5. L4 orchestrator: discover + dispatch */
    linqu::UnixSocketTransport l4_transport(TEST_BASE, l4_coord, 4);
    { bool _ok = l4_transport.start_listening(); if (!_ok) { fprintf(stderr, "[FATAL] L4 start_listening failed\n"); return 1; } }

    /* third arg = self_level (L4=4), NOT num_expected_completions */
    linqu::RemoteDispatcher dispatcher(l4_transport, l4_coord, 4);
    dispatcher.start_recv_loop();

    linqu::PeerRegistry   registry;
    linqu::FilesystemDiscovery disco(TEST_BASE);
    disco.discover(registry);
    auto l3_peers = registry.peers_at_level(3);
    fprintf(stderr, "[L4] Discovered %zu L3 peers\n", l3_peers.size());
    assert((int)l3_peers.size() == NUM_L3);

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
        linqu_submit_task(rt, target, "dfs_L3_reader.so", nullptr, 0);
    }
    linqu_scope_end(rt);
    dispatcher.wait_all();
    dispatcher.stop_recv_loop();
    l4_transport.stop();

    /* 6. Collect child exit codes */
    for (auto cpid : children) {
        int ws;
        waitpid(cpid, &ws, 0);
        assert(WIFEXITED(ws) && WEXITSTATUS(ws) == 0);
    }

    /* 7. Read sum.json files and aggregate */
    fprintf(stderr, "\n=== Verification ===\n");
    uint64_t computed_total = 0;
    int      sum_file_count = 0;

    for (int i = 0; i < NUM_L3; i++) {
        std::string path = std::string(DFS_OUTPUT) +
                           "/L3_" + std::to_string(i) + "/sum.json";
        uint64_t local_sum = read_local_sum(path);
        if (local_sum > 0 || /* zero is valid if file exists */
            [&]{ std::ifstream tf(path); return tf.good(); }()) {
            sum_file_count++;
            computed_total += local_sum;
            if (i < 4 || i == NUM_L3 - 1) {
                fprintf(stderr, "  L3[%2d] local_sum=%llu\n",
                        i, (unsigned long long)local_sum);
            } else if (i == 4) {
                fprintf(stderr, "  ... (%d more) ...\n", NUM_L3 - 5);
            }
        } else {
            fprintf(stderr, "  L3[%2d] sum.json MISSING [FAIL]\n", i);
        }
    }

    fprintf(stderr, "\n[CHECK 1] sum.json files: %d/%d\n",
            sum_file_count, NUM_L3);
    assert(sum_file_count == NUM_L3);

    fprintf(stderr, "[CHECK 2] computed_total=%llu expected_total=%llu\n",
            (unsigned long long)computed_total,
            (unsigned long long)expected_total);
    assert(computed_total == expected_total);

    fprintf(stderr, "[CHECK 3] tasks_submitted=%lld (expected %d)\n",
            (long long)state.tasks_submitted(), NUM_L3);
    assert(state.tasks_submitted() == NUM_L3);

    cleanup();

    fprintf(stderr, "\n=== DFS Sum Multiprocess Test PASSED ===\n");
    fprintf(stderr, "Verified:\n");
    fprintf(stderr, "  1. dfs_L3_reader.so compiled and loaded via dlopen\n");
    fprintf(stderr, "  2. linqu_orch_config().role == LINQU_ROLE_WORKER\n");
    fprintf(stderr,
            "  3. Each L3 daemon read lingqu_dfs file and wrote sum.json\n");
    fprintf(stderr, "  4. L4 orchestrator aggregated total matches expected\n");
    fprintf(stderr, "  5. %d multi-process dispatches completed correctly\n",
            NUM_L3);
    return 0;
}
