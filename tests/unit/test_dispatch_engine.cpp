#include "daemon/node_daemon.h"
#include "runtime/linqu_orchestrator_state.h"
#include "runtime/remote_dispatcher.h"
#include "discovery/peer_registry.h"
#include "discovery/filesystem_discovery.h"
#include "transport/unix_socket_transport.h"
#include "transport/msg_types.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fstream>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>

/*
 * End-to-end dispatch engine test:
 *   1. Build a test kernel .so that writes a file as proof of execution.
 *   2. Fork 2 L3 daemon processes.
 *   3. Parent (L4) uses PeerRegistry + RemoteDispatcher + OrchestratorState
 *      to dispatch the .so kernel to each L3 daemon.
 *   4. Each daemon dlopen()s the .so and executes it.
 *   5. Parent verifies completion and checks for proof files.
 */

static const char* TEST_BASE = "/tmp/linqu_test_engine";

static void cleanup() {
    (void)system(("rm -rf " + std::string(TEST_BASE)).c_str());
}

static std::string build_test_kernel() {
    std::string src_dir = std::string(TEST_BASE) + "/src";
    (void)system(("mkdir -p " + src_dir).c_str());

    std::string src = src_dir + "/proof_kernel.cpp";
    {
        std::ofstream f(src);
        f << "#include <cstdint>\n";
        f << "#include <cstdio>\n";
        f << "#include <cstdlib>\n";
        f << "#include <cstring>\n";
        f << "#include <fstream>\n";
        f << "#include <unistd.h>\n";
        f << "extern \"C\" {\n";
        f << "struct LinquOrchConfig { uint8_t level; int expected_arg_count; uint8_t role; };\n";
        f << "struct LinquRuntimeOps;\n";
        f << "struct LinquRuntime { const LinquRuntimeOps* ops; };\n";
        f << "__attribute__((visibility(\"default\")))\n";
        f << "LinquOrchConfig linqu_orch_config(uint64_t*, int) {\n";
        f << "  return {3, 0};\n";
        f << "}\n";
        f << "__attribute__((visibility(\"default\")))\n";
        f << "void linqu_orch_entry(LinquRuntime*, uint64_t*, int) {\n";
        f << "  char proof[256];\n";
        f << "  snprintf(proof, sizeof(proof), \"/tmp/linqu_test_engine/proof_%d.txt\", getpid());\n";
        f << "  std::ofstream out(proof);\n";
        f << "  out << \"executed_by_pid=\" << getpid() << \"\\n\";\n";
        f << "}\n";
        f << "}\n";
    }

    std::string so_path = src_dir + "/proof_kernel.so";
    std::string cmd = "g++ -shared -fPIC -o " + so_path + " " + src + " 2>&1";
    int rc = system(cmd.c_str());
    assert(rc == 0);
    return so_path;
}

static int run_l3_daemon(linqu::LinquCoordinate coord, const std::string& so_path) {
    linqu::NodeDaemon daemon(linqu::Level::HOST, coord, TEST_BASE);
    if (!daemon.start()) return 1;

    // Pre-register the kernel .so
    std::ifstream f(so_path, std::ios::binary | std::ios::ate);
    size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> data(sz);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sz));
    daemon.code_cache().register_code("proof_kernel.so", data.data(), data.size());

    daemon.run_event_loop(1);
    daemon.stop();
    return 0;
}

int main() {
    cleanup();
    fprintf(stderr, "=== Dispatch Engine End-to-End Test ===\n\n");

    // 1. Build the kernel .so
    std::string so_path = build_test_kernel();
    fprintf(stderr, "[E2E] Built kernel at %s\n", so_path.c_str());

    // 2. Fork 2 L3 daemons
    linqu::LinquCoordinate l4_coord;
    l4_coord.l5_idx = 0; l4_coord.l4_idx = 0;

    std::vector<pid_t> children;
    for (int i = 0; i < 2; i++) {
        linqu::LinquCoordinate l3_coord;
        l3_coord.l5_idx = 0; l3_coord.l4_idx = 0;
        l3_coord.l3_idx = static_cast<uint16_t>(i);

        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            _exit(run_l3_daemon(l3_coord, so_path));
        }
        children.push_back(pid);
        fprintf(stderr, "[E2E] Forked L3[%d] daemon pid=%d\n", i, pid);
    }

    usleep(600000);

    // 3. Parent: set up L4 transport + RemoteDispatcher
    linqu::UnixSocketTransport l4_transport(TEST_BASE, l4_coord, 4);
    { bool _ok = l4_transport.start_listening(); if (!_ok) { fprintf(stderr, "[FATAL] L4 start_listening failed\n"); return 1; } }

    linqu::RemoteDispatcher dispatcher(l4_transport, l4_coord, 4);
    dispatcher.start_recv_loop();

    // 4. Discover L3 daemons
    linqu::PeerRegistry registry;
    linqu::FilesystemDiscovery disco(TEST_BASE);
    int found = disco.discover(registry);
    fprintf(stderr, "[E2E] Discovered %d peers\n", found);

    auto l3_peers = registry.peers_at_level(3);
    assert(l3_peers.size() == 2);

    // 5. Set up OrchestratorState + dispatch
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
        linqu_submit_task(rt, target, "proof_kernel.so", nullptr, 0);
    }
    linqu_scope_end(rt);

    fprintf(stderr, "[E2E] Dispatched %lld tasks, waiting for completion...\n",
            (long long)state.tasks_submitted());

    dispatcher.wait_all();
    dispatcher.stop_recv_loop();
    l4_transport.stop();

    fprintf(stderr, "[E2E] All tasks completed.\n");

    // 6. Wait for children
    for (auto cpid : children) {
        int wstatus;
        waitpid(cpid, &wstatus, 0);
        assert(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);
    }

    // 7. Verify proof files exist (one per daemon PID)
    int proof_count = 0;
    for (auto cpid : children) {
        std::string proof = std::string(TEST_BASE) + "/proof_" +
                            std::to_string(cpid) + ".txt";
        std::ifstream pf(proof);
        if (pf.good()) {
            std::string line;
            std::getline(pf, line);
            fprintf(stderr, "[E2E] Proof file: %s → %s\n", proof.c_str(), line.c_str());
            assert(line.find("executed_by_pid=") == 0);
            proof_count++;
        } else {
            fprintf(stderr, "[E2E] WARNING: Proof file not found: %s\n", proof.c_str());
        }
    }

    assert(proof_count == 2);
    assert(state.tasks_submitted() == 2);

    cleanup();

    fprintf(stderr, "\n=== Dispatch Engine Test PASSED ===\n");
    fprintf(stderr, "Verified:\n");
    fprintf(stderr, "  1. Built and distributed .so kernel\n");
    fprintf(stderr, "  2. 2 L3 daemons started in separate processes\n");
    fprintf(stderr, "  3. FilesystemDiscovery found both daemons\n");
    fprintf(stderr, "  4. OrchestratorState + RemoteDispatcher dispatched to both\n");
    fprintf(stderr, "  5. Each daemon dlopen'd + executed the kernel\n");
    fprintf(stderr, "  6. Proof files confirm execution in each daemon's process\n");
    fprintf(stderr, "  7. TASK_COMPLETE received for all tasks\n");
    return 0;
}
