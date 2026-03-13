#include "daemon/node_daemon.h"
#include "daemon/code_cache.h"
#include "transport/unix_socket_transport.h"
#include "transport/msg_types.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <string>
#include <fstream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

static const char* TEST_BASE = "/tmp/linqu_test_daemon";

static void cleanup() {
    (void)system(("rm -rf " + std::string(TEST_BASE)).c_str());
}

// ================================================================
// Test 1: CodeCache register + load .so
// ================================================================
static void test_code_cache() {
    fprintf(stderr, "--- Test: CodeCache ---\n");
    cleanup();
    std::string store = std::string(TEST_BASE) + "/cache_test";

    linqu::CodeCache cache(store);
    assert(!cache.has("test.so"));

    // Build a minimal .so in memory
    // We compile a small .so from a temp C file
    std::string src_path = std::string(TEST_BASE) + "/test_kernel.cpp";
    (void)system(("mkdir -p " + std::string(TEST_BASE)).c_str());
    {
        std::ofstream f(src_path);
        f << "#include <cstdint>\n";
        f << "extern \"C\" {\n";
        f << "struct LinquOrchConfig { uint8_t level; int expected_arg_count; };\n";
        f << "struct LinquRuntime { const void* ops; };\n";
        f << "__attribute__((visibility(\"default\")))\n";
        f << "LinquOrchConfig linqu_orch_config(uint64_t*, int) {\n";
        f << "  return {3, 0};\n";
        f << "}\n";
        f << "static int g_called = 0;\n";
        f << "__attribute__((visibility(\"default\")))\n";
        f << "void linqu_orch_entry(LinquRuntime*, uint64_t*, int) {\n";
        f << "  g_called = 1;\n";
        f << "}\n";
        f << "__attribute__((visibility(\"default\")))\n";
        f << "int linqu_test_was_called() { return g_called; }\n";
        f << "}\n";
    }

    std::string so_path = std::string(TEST_BASE) + "/test_kernel.so";
    std::string cmd = "g++ -shared -fPIC -o " + so_path + " " + src_path + " 2>&1";
    int rc = system(cmd.c_str());
    assert(rc == 0);

    // Read .so binary
    std::ifstream f(so_path, std::ios::binary | std::ios::ate);
    size_t sz = static_cast<size_t>(f.tellg());
    f.seekg(0);
    std::vector<uint8_t> data(sz);
    f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(sz));

    cache.register_code("test_kernel.so", data.data(), data.size());
    assert(cache.has("test_kernel.so"));

    auto kernel = cache.load("test_kernel.so");
    assert(kernel.dl_handle != nullptr);
    assert(kernel.entry_fn != nullptr);
    assert(kernel.config_fn != nullptr);

    auto cfg = kernel.config_fn(nullptr, 0);
    assert(cfg.level == 3);
    assert(cfg.expected_arg_count == 0);

    kernel.entry_fn(nullptr, nullptr, 0);

    using TestFn = int(*)();
    auto was_called = reinterpret_cast<TestFn>(
        dlsym(kernel.dl_handle, "linqu_test_was_called"));
    assert(was_called && was_called() == 1);

    cache.unload_all();
    cleanup();

    fprintf(stderr, "  CodeCache: register, load, dlopen, dlsym, execute [PASS]\n");
}

// ================================================================
// Test 2: NodeDaemon handles CALL_TASK + SHUTDOWN in separate process
// ================================================================
static int run_daemon_child(linqu::LinquCoordinate coord) {
    linqu::NodeDaemon daemon(linqu::Level::HOST, coord, TEST_BASE);
    if (!daemon.start()) return 1;
    daemon.run_event_loop(2);
    daemon.stop();
    fprintf(stderr, "[daemon child] Handled %d messages\n",
            daemon.messages_handled());
    return (daemon.messages_handled() == 2) ? 0 : 1;
}

static bool send_raw(const std::string& sock_path,
                     const linqu::LinquHeader& hdr,
                     const uint8_t* payload, size_t len) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }
    uint8_t wire[linqu::LinquHeader::WIRE_SIZE];
    hdr.serialize(wire);
    (void)write(fd, wire, linqu::LinquHeader::WIRE_SIZE);
    if (len > 0) (void)write(fd, payload, len);
    close(fd);
    return true;
}

static void test_daemon_message_handling() {
    fprintf(stderr, "--- Test: NodeDaemon message handling ---\n");
    cleanup();

    linqu::LinquCoordinate l3_coord;
    l3_coord.l5_idx = 0; l3_coord.l4_idx = 0; l3_coord.l3_idx = 5;
    linqu::LinquCoordinate l4_coord;
    l4_coord.l5_idx = 0; l4_coord.l4_idx = 0;

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        _exit(run_daemon_child(l3_coord));
    }

    // Parent listens for TASK_COMPLETE
    linqu::UnixSocketTransport parent_transport(TEST_BASE, l4_coord, 4);
    assert(parent_transport.start_listening());
    usleep(500000);

    // Send CALL_TASK
    linqu::CallTaskPayload task;
    task.kernel_so_name = "nonexistent.so";
    task.task_id = 77;
    task.num_params = 0;
    auto buf = task.serialize();

    linqu::LinquHeader hdr = linqu::LinquHeader::make(
        linqu::MsgType::CALL_TASK,
        4, l4_coord, l3_coord,
        static_cast<uint32_t>(buf.size()));

    std::string daemon_sock = linqu::UnixSocketTransport::make_socket_path(
        TEST_BASE, l3_coord, 3);
    assert(send_raw(daemon_sock, hdr, buf.data(), buf.size()));
    fprintf(stderr, "[parent] Sent CALL_TASK\n");

    // Receive TASK_COMPLETE
    linqu::LinquHeader resp;
    std::vector<uint8_t> resp_payload;
    assert(parent_transport.recv(resp, resp_payload, 10000));
    assert(resp.msg_type == linqu::MsgType::TASK_COMPLETE);
    auto comp = linqu::TaskCompletePayload::deserialize(
        resp_payload.data(), resp_payload.size());
    assert(comp.task_id == 77);
    assert(comp.status == 0);
    fprintf(stderr, "[parent] Got TASK_COMPLETE for task_id=%u [PASS]\n",
            comp.task_id);

    // Send SHUTDOWN
    linqu::ShutdownPayload shut;
    auto shut_buf = shut.serialize();
    linqu::LinquHeader shut_hdr = linqu::LinquHeader::make(
        linqu::MsgType::SHUTDOWN,
        4, l4_coord, l3_coord,
        static_cast<uint32_t>(shut_buf.size()));
    assert(send_raw(daemon_sock, shut_hdr, shut_buf.data(), shut_buf.size()));

    int wstatus;
    waitpid(child, &wstatus, 0);
    assert(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);

    parent_transport.stop();
    cleanup();
    fprintf(stderr, "  NodeDaemon: handled CALL_TASK + SHUTDOWN, "
            "sent TASK_COMPLETE back [PASS]\n");
}

int main() {
    fprintf(stderr, "=== Daemon Tests ===\n\n");
    test_code_cache();
    test_daemon_message_handling();
    fprintf(stderr, "\n=== All Daemon Tests PASSED ===\n");
    return 0;
}
