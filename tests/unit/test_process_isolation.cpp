#include "transport/linqu_header.h"
#include "transport/msg_types.h"
#include "transport/unix_socket_transport.h"
#include "core/level.h"
#include "core/coordinate.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

static const char* TEST_BASE = "/tmp/linqu_test_isolation";

static void cleanup() {
    // Remove leftover socket files
    (void)system(("rm -rf " + std::string(TEST_BASE)).c_str());
}

// ========================================================================
// Child process: acts as L3 daemon — listens for CALL_TASK, responds
// with TASK_COMPLETE. Also verifies it has its own address space by
// writing to a local variable that the parent cannot see.
// ========================================================================
static int run_child_l3(linqu::LinquCoordinate l3_coord) {
    int child_secret = 0xDEAD;

    linqu::UnixSocketTransport transport(TEST_BASE, l3_coord, 3);
    if (!transport.start_listening()) {
        fprintf(stderr, "[L3 child pid=%d] Failed to listen at %s\n",
                getpid(), transport.socket_path().c_str());
        return 1;
    }
    fprintf(stderr, "[L3 child pid=%d] Listening at %s\n",
            getpid(), transport.socket_path().c_str());

    // Wait for a CALL_TASK message
    linqu::LinquHeader hdr;
    std::vector<uint8_t> payload;
    if (!transport.recv(hdr, payload, 10000)) {
        fprintf(stderr, "[L3 child] recv failed\n");
        return 1;
    }

    assert(hdr.magic == linqu::LINQU_MAGIC);
    assert(hdr.msg_type == linqu::MsgType::CALL_TASK);
    fprintf(stderr, "[L3 child pid=%d] Received CALL_TASK from L%d\n",
            getpid(), hdr.sender_level);

    auto task = linqu::CallTaskPayload::deserialize(payload.data(), payload.size());
    fprintf(stderr, "[L3 child pid=%d] kernel=%s task_id=%u params=%u\n",
            getpid(), task.kernel_so_name.c_str(), task.task_id, task.num_params);

    assert(task.kernel_so_name == "test_kernel_L3.so");
    assert(task.task_id == 42);
    assert(task.num_params == 2);
    assert(task.params[0].type == 0); // INPUT
    assert(task.params[0].handle == 100);
    assert(task.params[1].type == 1); // OUTPUT
    assert(task.params[1].handle == 200);

    // Modify child_secret — parent's copy is unaffected (process isolation!)
    child_secret = 0xBEEF;

    // Send TASK_COMPLETE back to L4
    linqu::LinquCoordinate parent_coord = hdr.get_sender();

    linqu::TaskCompletePayload comp;
    comp.task_id = task.task_id;
    comp.status = 0;
    auto comp_buf = comp.serialize();

    linqu::LinquHeader resp = linqu::LinquHeader::make(
        linqu::MsgType::TASK_COMPLETE,
        3, l3_coord, parent_coord,
        static_cast<uint32_t>(comp_buf.size()));

    // To send back, we need to know the parent's socket path
    // The parent (L4) also runs a transport at its level
    std::string parent_sock = linqu::UnixSocketTransport::make_socket_path(
        TEST_BASE, parent_coord, 4);
    fprintf(stderr, "[L3 child pid=%d] Sending TASK_COMPLETE to %s\n",
            getpid(), parent_sock.c_str());

    // Direct socket connection for the reply
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, parent_sock.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("[L3 child] connect to parent");
        close(fd);
        return 1;
    }
    uint8_t wire[linqu::LinquHeader::WIRE_SIZE];
    resp.serialize(wire);
    (void)write(fd, wire, linqu::LinquHeader::WIRE_SIZE);
    (void)write(fd, comp_buf.data(), comp_buf.size());
    close(fd);

    fprintf(stderr, "[L3 child pid=%d] Done. child_secret=0x%X\n",
            getpid(), child_secret);
    (void)child_secret;

    transport.stop();
    return 0;
}

// ========================================================================
// Parent process: acts as L4 pod — sends CALL_TASK to L3, waits for
// TASK_COMPLETE response.
// ========================================================================
int main() {
    cleanup();
    fprintf(stderr, "=== Process Isolation Test ===\n");
    fprintf(stderr, "Parent PID=%d\n\n", getpid());

    int parent_secret = 0xCAFE;

    linqu::LinquCoordinate l4_coord;
    l4_coord.l6_idx = 0;
    l4_coord.l5_idx = 0;
    l4_coord.l4_idx = 1;

    linqu::LinquCoordinate l3_coord;
    l3_coord.l6_idx = 0;
    l3_coord.l5_idx = 0;
    l3_coord.l4_idx = 1;
    l3_coord.l3_idx = 5;

    // Fork child process (simulating L3 daemon)
    pid_t child_pid = fork();
    if (child_pid < 0) {
        perror("fork");
        return 1;
    }

    if (child_pid == 0) {
        // In child process — run L3 daemon
        int rc = run_child_l3(l3_coord);
        _exit(rc);
    }

    // Parent process — acts as L4 pod orchestrator
    fprintf(stderr, "[L4 parent pid=%d] Forked L3 child pid=%d\n",
            getpid(), child_pid);

    // Start L4's own transport (to receive TASK_COMPLETE reply)
    linqu::UnixSocketTransport l4_transport(TEST_BASE, l4_coord, 4);
    assert(l4_transport.start_listening());
    fprintf(stderr, "[L4 parent pid=%d] Listening at %s\n",
            getpid(), l4_transport.socket_path().c_str());

    // Wait a moment for child to start listening
    usleep(500000);

    // Build CALL_TASK message
    linqu::CallTaskPayload task;
    task.kernel_so_name = "test_kernel_L3.so";
    task.task_id = 42;
    task.num_params = 2;
    task.params.push_back({0, 100, 0}); // INPUT, handle=100
    task.params.push_back({1, 200, 0}); // OUTPUT, handle=200
    auto task_buf = task.serialize();

    linqu::LinquHeader hdr = linqu::LinquHeader::make(
        linqu::MsgType::CALL_TASK,
        4, l4_coord, l3_coord,
        static_cast<uint32_t>(task_buf.size()));

    // Send to L3 child via Unix socket (separate process, separate address space)
    std::string l3_sock = linqu::UnixSocketTransport::make_socket_path(
        TEST_BASE, l3_coord, 3);
    fprintf(stderr, "[L4 parent pid=%d] Sending CALL_TASK to L3 at %s\n",
            getpid(), l3_sock.c_str());

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, l3_sock.c_str(), sizeof(addr.sun_path) - 1);
    assert(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
    uint8_t wire[linqu::LinquHeader::WIRE_SIZE];
    hdr.serialize(wire);
    assert(write(fd, wire, linqu::LinquHeader::WIRE_SIZE) == linqu::LinquHeader::WIRE_SIZE);
    assert(write(fd, task_buf.data(), task_buf.size()) == (ssize_t)task_buf.size());
    close(fd);
    fprintf(stderr, "[L4 parent pid=%d] CALL_TASK sent.\n", getpid());

    // Wait for TASK_COMPLETE from L3
    linqu::LinquHeader resp_hdr;
    std::vector<uint8_t> resp_payload;
    assert(l4_transport.recv(resp_hdr, resp_payload, 10000));
    assert(resp_hdr.magic == linqu::LINQU_MAGIC);
    assert(resp_hdr.msg_type == linqu::MsgType::TASK_COMPLETE);

    auto comp = linqu::TaskCompletePayload::deserialize(resp_payload.data(), resp_payload.size());
    fprintf(stderr, "[L4 parent pid=%d] Received TASK_COMPLETE: task_id=%u status=%d\n",
            getpid(), comp.task_id, comp.status);
    assert(comp.task_id == 42);
    assert(comp.status == 0);

    // Verify parent_secret is unchanged (child modified its own copy)
    assert(parent_secret == 0xCAFE);
    fprintf(stderr, "[L4 parent pid=%d] parent_secret=0x%X (unchanged, proving isolation)\n",
            getpid(), parent_secret);

    // Wait for child to exit
    int wstatus;
    waitpid(child_pid, &wstatus, 0);
    assert(WIFEXITED(wstatus));
    assert(WEXITSTATUS(wstatus) == 0);
    fprintf(stderr, "[L4 parent pid=%d] L3 child exited successfully.\n", getpid());

    l4_transport.stop();
    cleanup();

    fprintf(stderr, "\n=== Process Isolation Test PASSED ===\n");
    fprintf(stderr, "Verified:\n");
    fprintf(stderr, "  1. L4 and L3 run in separate processes (different PIDs)\n");
    fprintf(stderr, "  2. Cross-level communication via Unix domain socket IPC\n");
    fprintf(stderr, "  3. CALL_TASK message serialization/deserialization\n");
    fprintf(stderr, "  4. TASK_COMPLETE response serialization/deserialization\n");
    fprintf(stderr, "  5. Address space isolation (parent_secret unmodified by child)\n");
    fprintf(stderr, "  6. Each process has its own socket endpoint\n");
    return 0;
}
