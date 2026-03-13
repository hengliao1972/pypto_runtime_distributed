#include "transport/msg_types.h"
#include "runtime/linqu_orchestration_api.h"
#include "runtime/linqu_orchestrator_state.h"
#include "runtime/linqu_dispatcher.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace linqu;

static void test_param_serialization_roundtrip() {
    CallTaskPayload out;
    out.kernel_so_name = "matmul_kernel.so";
    out.task_id = 42;
    out.scope_depth = 3;
    out.num_params = 4;

    CallTaskPayload::ParamEntry e0;
    e0.type = LINQU_PARAM_INPUT;
    e0.handle = 100;
    e0.scalar_value = 0;
    out.params.push_back(e0);

    CallTaskPayload::ParamEntry e1;
    e1.type = LINQU_PARAM_INPUT;
    e1.handle = 200;
    e1.scalar_value = 0;
    out.params.push_back(e1);

    CallTaskPayload::ParamEntry e2;
    e2.type = LINQU_PARAM_OUTPUT;
    e2.handle = 300;
    e2.scalar_value = 0;
    out.params.push_back(e2);

    CallTaskPayload::ParamEntry e3;
    e3.type = LINQU_PARAM_SCALAR;
    e3.handle = 0;
    e3.scalar_value = 0xDEADBEEFCAFE;
    out.params.push_back(e3);

    auto buf = out.serialize();
    auto in = CallTaskPayload::deserialize(buf.data(), buf.size());

    assert(in.kernel_so_name == "matmul_kernel.so");
    assert(in.task_id == 42);
    assert(in.scope_depth == 3);
    assert(in.num_params == 4);
    assert(in.params.size() == 4);

    assert(in.params[0].type == LINQU_PARAM_INPUT);
    assert(in.params[0].handle == 100);

    assert(in.params[1].type == LINQU_PARAM_INPUT);
    assert(in.params[1].handle == 200);

    assert(in.params[2].type == LINQU_PARAM_OUTPUT);
    assert(in.params[2].handle == 300);

    assert(in.params[3].type == LINQU_PARAM_SCALAR);
    assert(in.params[3].scalar_value == 0xDEADBEEFCAFE);

    printf("  param_serialization_roundtrip: PASS\n");
}

static void test_param_to_args_conversion() {
    CallTaskPayload::ParamEntry entries[3];
    entries[0].type = LINQU_PARAM_INPUT;
    entries[0].handle = 111;
    entries[0].scalar_value = 0;

    entries[1].type = LINQU_PARAM_OUTPUT;
    entries[1].handle = 222;
    entries[1].scalar_value = 0;

    entries[2].type = LINQU_PARAM_SCALAR;
    entries[2].handle = 0;
    entries[2].scalar_value = 999;

    std::vector<uint64_t> args;
    for (const auto& pe : entries) {
        if (pe.type == LINQU_PARAM_SCALAR) {
            args.push_back(pe.scalar_value);
        } else {
            args.push_back(pe.handle);
        }
    }

    assert(args.size() == 3);
    assert(args[0] == 111);
    assert(args[1] == 222);
    assert(args[2] == 999);

    printf("  param_to_args_conversion: PASS\n");
}

class ParamCapture : public LinquDispatcher {
public:
    bool dispatch(int32_t task_id, const LinquCoordinate&,
                  const std::string& kernel_so,
                  LinquParam* params, int num_params) override {
        for (int i = 0; i < num_params; i++) {
            captured_params_.push_back(params[i]);
        }
        DispatchResult r;
        r.task_id = task_id;
        r.status = TaskStatus::COMPLETED;
        notify_completion(r);
        return true;
    }
    void wait_all() override {}

    std::vector<LinquParam> captured_params_;
};

static void test_dispatcher_receives_params() {
    LinquOrchestratorState state;
    LinquOrchConfig_Internal cfg;
    cfg.task_ring_window = 8;
    cfg.heap_ring_capacity = 65536;
    state.init(Level::POD, LinquCoordinate{}, cfg);

    ParamCapture capture;
    state.set_dispatcher(&capture);

    LinquRuntime* rt = state.runtime();
    LinquCoordinate_C target{};

    uint64_t h_a = linqu_alloc_tensor(rt, target, 1024);
    uint64_t h_b = linqu_alloc_tensor(rt, target, 1024);
    uint64_t h_c = linqu_alloc_tensor(rt, target, 1024);

    linqu_scope_begin(rt);

    LinquParam params[] = {
        linqu_make_input(h_a),
        linqu_make_input(h_b),
        linqu_make_output(h_c),
        linqu_make_scalar(42),
    };
    linqu_submit_task(rt, target, "k.so", params, 4);

    linqu_scope_end(rt);

    assert(capture.captured_params_.size() == 4);
    assert(capture.captured_params_[0].type == LINQU_PARAM_INPUT);
    assert(capture.captured_params_[0].handle == h_a);
    assert(capture.captured_params_[1].type == LINQU_PARAM_INPUT);
    assert(capture.captured_params_[1].handle == h_b);
    assert(capture.captured_params_[2].type == LINQU_PARAM_OUTPUT);
    assert(capture.captured_params_[2].handle == h_c);
    assert(capture.captured_params_[3].type == LINQU_PARAM_SCALAR);
    assert(capture.captured_params_[3].scalar_value == 42);

    printf("  dispatcher_receives_params: PASS\n");
}

static void test_remote_dispatcher_serializes_params() {
    LinquParam params[] = {
        linqu_make_input(10),
        linqu_make_output(20),
        linqu_make_inout(30),
        linqu_make_scalar(0xAAAA),
    };

    CallTaskPayload payload;
    payload.kernel_so_name = "test.so";
    payload.task_id = 7;
    payload.scope_depth = 1;
    payload.num_params = 4;
    for (int i = 0; i < 4; i++) {
        CallTaskPayload::ParamEntry e;
        e.type = static_cast<uint8_t>(params[i].type);
        e.handle = params[i].handle;
        e.scalar_value = params[i].scalar_value;
        payload.params.push_back(e);
    }

    auto buf = payload.serialize();
    auto decoded = CallTaskPayload::deserialize(buf.data(), buf.size());

    assert(decoded.num_params == 4);
    assert(decoded.params[0].type == LINQU_PARAM_INPUT);
    assert(decoded.params[0].handle == 10);
    assert(decoded.params[1].type == LINQU_PARAM_OUTPUT);
    assert(decoded.params[1].handle == 20);
    assert(decoded.params[2].type == LINQU_PARAM_INOUT);
    assert(decoded.params[2].handle == 30);
    assert(decoded.params[3].type == LINQU_PARAM_SCALAR);
    assert(decoded.params[3].scalar_value == 0xAAAA);

    printf("  remote_dispatcher_serializes_params: PASS\n");
}

int main() {
    printf("=== test_param_passing ===\n");
    test_param_serialization_roundtrip();
    test_param_to_args_conversion();
    test_dispatcher_receives_params();
    test_remote_dispatcher_serializes_params();
    printf("=== ALL PASS ===\n");
    return 0;
}
