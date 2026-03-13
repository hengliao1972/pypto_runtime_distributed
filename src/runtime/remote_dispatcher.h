#ifndef LINQU_RUNTIME_REMOTE_DISPATCHER_H
#define LINQU_RUNTIME_REMOTE_DISPATCHER_H

#include "runtime/linqu_dispatcher.h"
#include "transport/unix_socket_transport.h"
#include "transport/msg_types.h"
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

namespace linqu {

class RemoteDispatcher : public LinquDispatcher {
public:
    RemoteDispatcher(UnixSocketTransport& transport,
                     const LinquCoordinate& self_coord,
                     uint8_t self_level);
    ~RemoteDispatcher() override;

    bool dispatch(int32_t task_id,
                  const LinquCoordinate& target,
                  const std::string& kernel_so,
                  LinquParam* params, int num_params) override;

    void wait_all() override;

    void start_recv_loop();
    void stop_recv_loop();

private:
    void recv_thread_fn();

    UnixSocketTransport& transport_;
    LinquCoordinate self_coord_;
    uint8_t self_level_;

    std::thread recv_thread_;
    std::atomic<bool> running_{false};

    std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_map<int32_t, TaskStatus> pending_;
    int32_t outstanding_ = 0;
};

}

#endif
