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
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

/*
 * Verifies 4-level process hierarchy: L6 → L5 → L4 → L3
 *
 * Each level runs in its own process. A CALL_TASK message propagates
 * down the chain. The L3 leaf sends TASK_COMPLETE back up through
 * each level. Every level appends its PID to a "trace" payload,
 * proving each hop is a separate process.
 *
 * Process tree:
 *   L6 (pid A) -- fork --> L5 (pid B) -- fork --> L4 (pid C) -- fork --> L3 (pid D)
 */

static const char* TEST_BASE = "/tmp/linqu_test_chain";

static void cleanup() {
    (void)system(("rm -rf " + std::string(TEST_BASE)).c_str());
}

static bool send_msg(const std::string& sock_path,
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
    ssize_t n = write(fd, wire, linqu::LinquHeader::WIRE_SIZE);
    if (n != linqu::LinquHeader::WIRE_SIZE) { close(fd); return false; }
    if (len > 0 && payload) {
        n = write(fd, payload, len);
        if (n != (ssize_t)len) { close(fd); return false; }
    }
    close(fd);
    return true;
}

static bool recv_msg(linqu::UnixSocketTransport& transport,
                     linqu::LinquHeader& hdr,
                     std::vector<uint8_t>& payload) {
    return transport.recv(hdr, payload, 15000);
}

/*
 * Each intermediate level (L5, L4) does:
 *   1. Listen on its own socket
 *   2. Fork child for the next lower level
 *   3. Receive CALL_TASK from parent
 *   4. Forward CALL_TASK to child (next level down)
 *   5. Receive TASK_COMPLETE from child
 *   6. Append own PID to the trace, forward TASK_COMPLETE to parent
 *
 * L3 (leaf): receives CALL_TASK, sends TASK_COMPLETE with its PID.
 * L6 (root): sends CALL_TASK, receives TASK_COMPLETE with all PIDs.
 */

struct LevelSpec {
    uint8_t level;
    linqu::LinquCoordinate coord;
};

static int build_trace_payload(pid_t pid, const std::vector<uint8_t>& child_trace,
                               std::vector<uint8_t>& out) {
    out = child_trace;
    uint32_t p = static_cast<uint32_t>(pid);
    uint32_t p_net = htonl(p);
    size_t old = out.size();
    out.resize(old + 4);
    memcpy(out.data() + old, &p_net, 4);
    return 0;
}

static std::vector<pid_t> parse_trace(const std::vector<uint8_t>& data) {
    std::vector<pid_t> pids;
    for (size_t i = 0; i + 4 <= data.size(); i += 4) {
        uint32_t p_net;
        memcpy(&p_net, data.data() + i, 4);
        pids.push_back(static_cast<pid_t>(ntohl(p_net)));
    }
    return pids;
}

static int run_leaf_l3(const LevelSpec& spec, const LevelSpec& parent) {
    linqu::UnixSocketTransport transport(TEST_BASE, spec.coord, spec.level);
    assert(transport.start_listening());
    fprintf(stderr, "[L%d pid=%d] Listening at %s\n",
            spec.level, getpid(), transport.socket_path().c_str());

    linqu::LinquHeader hdr;
    std::vector<uint8_t> payload;
    assert(recv_msg(transport, hdr, payload));
    assert(hdr.msg_type == linqu::MsgType::CALL_TASK);
    fprintf(stderr, "[L%d pid=%d] Received CALL_TASK, processing locally\n",
            spec.level, getpid());

    std::vector<uint8_t> trace;
    uint32_t p = htonl(static_cast<uint32_t>(getpid()));
    trace.resize(4);
    memcpy(trace.data(), &p, 4);

    linqu::LinquHeader resp = linqu::LinquHeader::make(
        linqu::MsgType::TASK_COMPLETE,
        spec.level, spec.coord, parent.coord,
        static_cast<uint32_t>(trace.size()));

    std::string parent_sock = linqu::UnixSocketTransport::make_socket_path(
        TEST_BASE, parent.coord, parent.level);
    assert(send_msg(parent_sock, resp, trace.data(), trace.size()));
    fprintf(stderr, "[L%d pid=%d] Sent TASK_COMPLETE (trace: pid=%d)\n",
            spec.level, getpid(), getpid());

    transport.stop();
    return 0;
}

static int run_intermediate(const LevelSpec& spec, const LevelSpec& parent,
                            const LevelSpec& child_spec, bool is_root);

static int run_intermediate(const LevelSpec& spec, const LevelSpec& parent,
                            const LevelSpec& child_spec, bool is_root) {
    linqu::UnixSocketTransport transport(TEST_BASE, spec.coord, spec.level);
    assert(transport.start_listening());
    fprintf(stderr, "[L%d pid=%d] Listening at %s\n",
            spec.level, getpid(), transport.socket_path().c_str());

    // Fork child for the next lower level
    LevelSpec grandchild;
    bool child_is_leaf = (child_spec.level == 3);

    pid_t child_pid = fork();
    assert(child_pid >= 0);

    if (child_pid == 0) {
        if (child_is_leaf) {
            _exit(run_leaf_l3(child_spec, spec));
        } else {
            grandchild.level = child_spec.level - 1;
            grandchild.coord = child_spec.coord;
            if (grandchild.level == 3) {
                grandchild.coord.l3_idx = 7;
            }
            _exit(run_intermediate(child_spec, spec, grandchild, false));
        }
    }

    fprintf(stderr, "[L%d pid=%d] Forked L%d child pid=%d\n",
            spec.level, getpid(), child_spec.level, child_pid);
    usleep(300000);

    if (is_root) {
        // Root (L6): send initial CALL_TASK to child
        linqu::CallTaskPayload task;
        task.kernel_so_name = "chain_test.so";
        task.task_id = 99;
        task.num_params = 0;
        auto task_buf = task.serialize();

        linqu::LinquHeader hdr = linqu::LinquHeader::make(
            linqu::MsgType::CALL_TASK,
            spec.level, spec.coord, child_spec.coord,
            static_cast<uint32_t>(task_buf.size()));

        std::string child_sock = linqu::UnixSocketTransport::make_socket_path(
            TEST_BASE, child_spec.coord, child_spec.level);
        fprintf(stderr, "[L%d pid=%d] Sending CALL_TASK to L%d at %s\n",
                spec.level, getpid(), child_spec.level, child_sock.c_str());
        assert(send_msg(child_sock, hdr, task_buf.data(), task_buf.size()));
    } else {
        // Intermediate: receive from parent, forward to child
        linqu::LinquHeader hdr;
        std::vector<uint8_t> payload;
        assert(recv_msg(transport, hdr, payload));
        assert(hdr.msg_type == linqu::MsgType::CALL_TASK);
        fprintf(stderr, "[L%d pid=%d] Received CALL_TASK from L%d, forwarding to L%d\n",
                spec.level, getpid(), hdr.sender_level, child_spec.level);

        linqu::LinquHeader fwd = linqu::LinquHeader::make(
            linqu::MsgType::CALL_TASK,
            spec.level, spec.coord, child_spec.coord,
            static_cast<uint32_t>(payload.size()));

        std::string child_sock = linqu::UnixSocketTransport::make_socket_path(
            TEST_BASE, child_spec.coord, child_spec.level);
        assert(send_msg(child_sock, fwd, payload.data(), payload.size()));
    }

    // Receive TASK_COMPLETE from child
    linqu::LinquHeader comp_hdr;
    std::vector<uint8_t> comp_payload;
    assert(recv_msg(transport, comp_hdr, comp_payload));
    assert(comp_hdr.msg_type == linqu::MsgType::TASK_COMPLETE);
    fprintf(stderr, "[L%d pid=%d] Received TASK_COMPLETE from L%d\n",
            spec.level, getpid(), comp_hdr.sender_level);

    // Append our PID to the trace
    std::vector<uint8_t> enriched_trace;
    build_trace_payload(getpid(), comp_payload, enriched_trace);

    if (!is_root) {
        // Forward enriched trace to parent
        linqu::LinquHeader resp = linqu::LinquHeader::make(
            linqu::MsgType::TASK_COMPLETE,
            spec.level, spec.coord, parent.coord,
            static_cast<uint32_t>(enriched_trace.size()));

        std::string parent_sock = linqu::UnixSocketTransport::make_socket_path(
            TEST_BASE, parent.coord, parent.level);
        assert(send_msg(parent_sock, resp, enriched_trace.data(), enriched_trace.size()));
        fprintf(stderr, "[L%d pid=%d] Forwarded TASK_COMPLETE to L%d\n",
                spec.level, getpid(), parent.level);
    } else {
        // Root: verify the trace
        auto pids = parse_trace(enriched_trace);
        fprintf(stderr, "\n[L6 ROOT pid=%d] === Trace of PIDs through hierarchy ===\n", getpid());
        const char* labels[] = {"L3", "L4", "L5", "L6"};
        for (size_t i = 0; i < pids.size(); i++) {
            fprintf(stderr, "  %s: pid=%d\n", labels[i], pids[i]);
        }

        // Verify all PIDs are distinct (proving separate processes)
        assert(pids.size() == 4);
        for (size_t i = 0; i < pids.size(); i++) {
            for (size_t j = i + 1; j < pids.size(); j++) {
                assert(pids[i] != pids[j]);
            }
        }
        fprintf(stderr, "  All 4 PIDs are distinct — process isolation confirmed!\n");
    }

    // Wait for child
    int wstatus;
    waitpid(child_pid, &wstatus, 0);
    assert(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);

    transport.stop();
    return 0;
}

int main() {
    cleanup();
    fprintf(stderr, "=== Multi-Level Process Chain Test (L6→L5→L4→L3) ===\n");
    fprintf(stderr, "Root PID=%d\n\n", getpid());

    linqu::LinquCoordinate base_coord;
    base_coord.l6_idx = 0;
    base_coord.l5_idx = 2;
    base_coord.l4_idx = 1;
    base_coord.l3_idx = 7;

    LevelSpec l6 = {6, base_coord};
    LevelSpec l5 = {5, base_coord};

    // This process is L6 (root)
    int rc = run_intermediate(l6, l6 /* unused for root */, l5, true);

    cleanup();
    if (rc == 0) {
        fprintf(stderr, "\n=== Multi-Level Process Chain Test PASSED ===\n");
        fprintf(stderr, "Verified:\n");
        fprintf(stderr, "  1. L6, L5, L4, L3 each run in a separate process\n");
        fprintf(stderr, "  2. CALL_TASK propagates L6→L5→L4→L3 via IPC\n");
        fprintf(stderr, "  3. TASK_COMPLETE propagates L3→L4→L5→L6 via IPC\n");
        fprintf(stderr, "  4. Each process appends its PID to the trace\n");
        fprintf(stderr, "  5. All 4 PIDs are distinct (address space isolation)\n");
    }
    return rc;
}
