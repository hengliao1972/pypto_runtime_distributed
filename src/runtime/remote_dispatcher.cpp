#include "runtime/remote_dispatcher.h"
#include <cstdio>

namespace linqu {

RemoteDispatcher::RemoteDispatcher(UnixSocketTransport& transport,
                                   const LinquCoordinate& self_coord,
                                   uint8_t self_level)
    : transport_(transport), self_coord_(self_coord), self_level_(self_level) {}

RemoteDispatcher::~RemoteDispatcher() {
    stop_recv_loop();
}

bool RemoteDispatcher::dispatch(int32_t task_id,
                                 const LinquCoordinate& target,
                                 const std::string& kernel_so,
                                 LinquParam* params, int num_params) {
    CallTaskPayload payload;
    payload.kernel_so_name = kernel_so;
    payload.task_id = static_cast<uint32_t>(task_id);
    payload.scope_depth = 0;
    payload.num_params = static_cast<uint32_t>(num_params);
    for (int i = 0; i < num_params; i++) {
        CallTaskPayload::ParamEntry e;
        e.type = static_cast<uint8_t>(params[i].type);
        e.handle = params[i].handle;
        e.scalar_value = params[i].scalar_value;
        payload.params.push_back(e);
    }
    auto buf = payload.serialize();

    LinquHeader hdr = LinquHeader::make(
        MsgType::CALL_TASK,
        self_level_, self_coord_, target,
        static_cast<uint32_t>(buf.size()));

    {
        std::lock_guard<std::mutex> lk(mu_);
        pending_[task_id] = TaskStatus::PENDING;
        outstanding_++;
    }

    bool ok = transport_.send(target, hdr, buf.data(), buf.size());
    if (!ok) {
        fprintf(stderr, "[RemoteDispatcher] Failed to send CALL_TASK for task %d to %s\n",
                task_id, target.to_string().c_str());
        std::lock_guard<std::mutex> lk(mu_);
        pending_[task_id] = TaskStatus::FAILED;
        outstanding_--;
        DispatchResult r;
        r.task_id = task_id;
        r.status = TaskStatus::FAILED;
        r.error_code = -1;
        notify_completion(r);
    }
    return ok;
}

void RemoteDispatcher::wait_all() {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait(lk, [this]{ return outstanding_ <= 0; });
}

void RemoteDispatcher::start_recv_loop() {
    if (running_) return;
    running_ = true;
    recv_thread_ = std::thread(&RemoteDispatcher::recv_thread_fn, this);
}

void RemoteDispatcher::stop_recv_loop() {
    running_ = false;
    if (recv_thread_.joinable()) {
        recv_thread_.join();
    }
}

void RemoteDispatcher::recv_thread_fn() {
    while (running_) {
        LinquHeader hdr;
        std::vector<uint8_t> payload;
        if (!transport_.recv(hdr, payload, 500)) {
            continue;
        }
        if (hdr.msg_type == MsgType::TASK_COMPLETE) {
            auto comp = TaskCompletePayload::deserialize(payload.data(), payload.size());
            int32_t tid = static_cast<int32_t>(comp.task_id);
            TaskStatus st = (comp.status == 0) ? TaskStatus::COMPLETED : TaskStatus::FAILED;

            DispatchResult r;
            r.task_id = tid;
            r.status = st;
            r.error_code = comp.status;
            notify_completion(r);

            {
                std::lock_guard<std::mutex> lk(mu_);
                pending_[tid] = st;
                outstanding_--;
            }
            cv_.notify_all();
        }
    }
}

}
