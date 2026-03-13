#include "runtime/linqu_dispatcher.h"
#include "runtime/mock_dispatcher.h"
#include "runtime/remote_dispatcher.h"
#include "runtime/linqu_orchestrator_state.h"
#include "transport/unix_socket_transport.h"
#include "transport/msg_types.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>

static const char* TEST_BASE = "/tmp/linqu_test_dispatch";

static void cleanup() {
    (void)system(("rm -rf " + std::string(TEST_BASE)).c_str());
}

// ================================================================
// Test 1: MockDispatcher records dispatches correctly
// ================================================================
static void test_mock_dispatcher() {
    fprintf(stderr, "--- Test: MockDispatcher ---\n");

    linqu::MockDispatcher mock;
    std::vector<linqu::DispatchResult> results;
    mock.set_completion_callback([&](const linqu::DispatchResult& r) {
        results.push_back(r);
    });

    linqu::LinquCoordinate target;
    target.l3_idx = 5;
    LinquParam params[2];
    params[0] = linqu_make_input(100);
    params[1] = linqu_make_output(200);

    assert(mock.dispatch(1, target, "kernel_add.so", params, 2));
    assert(mock.dispatch(2, target, "kernel_mul.so", params, 2));
    mock.wait_all();

    auto recs = mock.records();
    assert(recs.size() == 2);
    assert(recs[0].task_id == 1);
    assert(recs[0].kernel_so == "kernel_add.so");
    assert(recs[0].target.l3_idx == 5);
    assert(recs[0].num_params == 2);
    assert(recs[1].task_id == 2);
    assert(recs[1].kernel_so == "kernel_mul.so");

    assert(results.size() == 2);
    assert(results[0].status == linqu::TaskStatus::COMPLETED);
    assert(results[1].status == linqu::TaskStatus::COMPLETED);

    fprintf(stderr, "  MockDispatcher: 2 dispatches recorded, 2 completions received [PASS]\n");
}

// ================================================================
// Test 2: MockDispatcher integrated with LinquOrchestratorState
// ================================================================
static void test_orchestrator_with_mock_dispatcher() {
    fprintf(stderr, "--- Test: OrchestratorState + MockDispatcher ---\n");

    linqu::MockDispatcher mock;

    linqu::LinquOrchestratorState state;
    linqu::LinquOrchConfig_Internal cfg;
    linqu::LinquCoordinate coord;
    coord.l4_idx = 1;
    state.init(linqu::Level::POD, coord, cfg);
    state.set_dispatcher(&mock);

    LinquRuntime* rt = state.runtime();

    LinquCoordinate_C t0 = {}; t0.l3_idx = 0;
    LinquCoordinate_C t1 = {}; t1.l3_idx = 1;
    LinquCoordinate_C t2 = {}; t2.l3_idx = 2;

    uint64_t ha = linqu_alloc_tensor(rt, t0, 1024);
    uint64_t hb = linqu_alloc_tensor(rt, t0, 1024);
    uint64_t hc = linqu_alloc_tensor(rt, t0, 1024);

    LinquParam p0[3];
    p0[0] = linqu_make_input(ha);
    p0[1] = linqu_make_input(hb);
    p0[2] = linqu_make_output(hc);

    linqu_scope_begin(rt);
    linqu_submit_task(rt, t0, "add.so", p0, 3);

    LinquParam p1[2];
    p1[0] = linqu_make_input(hc);
    uint64_t hd = linqu_alloc_tensor(rt, t1, 1024);
    p1[1] = linqu_make_output(hd);
    linqu_submit_task(rt, t1, "scale.so", p1, 2);

    LinquParam p2[2];
    p2[0] = linqu_make_input(hd);
    uint64_t he = linqu_alloc_tensor(rt, t2, 1024);
    p2[1] = linqu_make_output(he);
    linqu_submit_task(rt, t2, "relu.so", p2, 2);
    linqu_scope_end(rt);

    linqu_orchestration_done(rt);

    auto recs = mock.records();
    assert(recs.size() == 3);
    assert(recs[0].kernel_so == "add.so");
    assert(recs[0].target.l3_idx == 0);
    assert(recs[1].kernel_so == "scale.so");
    assert(recs[1].target.l3_idx == 1);
    assert(recs[2].kernel_so == "relu.so");
    assert(recs[2].target.l3_idx == 2);

    assert(state.tasks_submitted() == 3);

    fprintf(stderr, "  OrchestratorState dispatched 3 tasks via MockDispatcher to 3 targets [PASS]\n");
}

// ================================================================
// Test 3: RemoteDispatcher sends CALL_TASK, child replies TASK_COMPLETE
// ================================================================
static int run_child_responder(linqu::LinquCoordinate child_coord) {
    linqu::UnixSocketTransport transport(TEST_BASE, child_coord, 3);
    if (!transport.start_listening()) {
        fprintf(stderr, "[child] Failed to listen\n");
        return 1;
    }

    for (int i = 0; i < 2; i++) {
        linqu::LinquHeader hdr;
        std::vector<uint8_t> payload;
        if (!transport.recv(hdr, payload, 10000)) {
            fprintf(stderr, "[child] recv failed for msg %d\n", i);
            return 1;
        }
        assert(hdr.msg_type == linqu::MsgType::CALL_TASK);
        auto task = linqu::CallTaskPayload::deserialize(payload.data(), payload.size());
        fprintf(stderr, "[child pid=%d] Got CALL_TASK: task_id=%u kernel=%s\n",
                getpid(), task.task_id, task.kernel_so_name.c_str());

        linqu::TaskCompletePayload comp;
        comp.task_id = task.task_id;
        comp.status = 0;
        auto comp_buf = comp.serialize();

        linqu::LinquHeader resp = linqu::LinquHeader::make(
            linqu::MsgType::TASK_COMPLETE,
            3, child_coord, hdr.get_sender(),
            static_cast<uint32_t>(comp_buf.size()));

        std::string parent_sock = linqu::UnixSocketTransport::make_socket_path(
            TEST_BASE, hdr.get_sender(), hdr.sender_level);

        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, parent_sock.c_str(), sizeof(addr.sun_path) - 1);
        assert(connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0);
        uint8_t wire[linqu::LinquHeader::WIRE_SIZE];
        resp.serialize(wire);
        (void)write(fd, wire, linqu::LinquHeader::WIRE_SIZE);
        (void)write(fd, comp_buf.data(), comp_buf.size());
        close(fd);
    }

    transport.stop();
    return 0;
}

static void test_remote_dispatcher() {
    fprintf(stderr, "--- Test: RemoteDispatcher (cross-process) ---\n");
    cleanup();

    linqu::LinquCoordinate l4_coord;
    l4_coord.l4_idx = 0;
    linqu::LinquCoordinate l3_coord;
    l3_coord.l4_idx = 0;
    l3_coord.l3_idx = 3;

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        _exit(run_child_responder(l3_coord));
    }

    linqu::UnixSocketTransport l4_transport(TEST_BASE, l4_coord, 4);
    assert(l4_transport.start_listening());
    usleep(500000);

    linqu::RemoteDispatcher remote(l4_transport, l4_coord, 4);
    remote.start_recv_loop();

    std::vector<linqu::DispatchResult> completions;
    std::mutex mu;
    remote.set_completion_callback([&](const linqu::DispatchResult& r) {
        std::lock_guard<std::mutex> lk(mu);
        completions.push_back(r);
    });

    LinquParam params[1];
    params[0] = linqu_make_scalar(42);

    assert(remote.dispatch(10, l3_coord, "kernel_A.so", params, 1));
    assert(remote.dispatch(11, l3_coord, "kernel_B.so", params, 1));

    remote.wait_all();
    remote.stop_recv_loop();

    {
        std::lock_guard<std::mutex> lk(mu);
        assert(completions.size() == 2);
        bool found_10 = false, found_11 = false;
        for (auto& c : completions) {
            assert(c.status == linqu::TaskStatus::COMPLETED);
            if (c.task_id == 10) found_10 = true;
            if (c.task_id == 11) found_11 = true;
        }
        assert(found_10 && found_11);
    }

    int wstatus;
    waitpid(child, &wstatus, 0);
    assert(WIFEXITED(wstatus) && WEXITSTATUS(wstatus) == 0);

    l4_transport.stop();
    cleanup();

    fprintf(stderr, "  RemoteDispatcher: 2 tasks dispatched to child process, "
            "2 completions received [PASS]\n");
}

int main() {
    fprintf(stderr, "=== Dispatcher Tests ===\n\n");
    test_mock_dispatcher();
    test_orchestrator_with_mock_dispatcher();
    test_remote_dispatcher();
    fprintf(stderr, "\n=== All Dispatcher Tests PASSED ===\n");
    return 0;
}
