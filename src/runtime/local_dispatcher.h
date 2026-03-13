#ifndef LINQU_RUNTIME_LOCAL_DISPATCHER_H
#define LINQU_RUNTIME_LOCAL_DISPATCHER_H

#include "runtime/linqu_dispatcher.h"
#include <mutex>
#include <atomic>

namespace linqu {

class LocalDispatcher : public LinquDispatcher {
public:
    LocalDispatcher();
    ~LocalDispatcher() override;

    bool dispatch(int32_t task_id,
                  const LinquCoordinate& target,
                  const std::string& kernel_so,
                  LinquParam* params, int num_params) override;

    void wait_all() override;

    void set_stub_latency_us(int us) { stub_latency_us_ = us; }

    int dispatched_count() const { return dispatched_count_.load(); }

private:
    std::mutex mu_;
    std::atomic<int> dispatched_count_{0};
    std::atomic<int> outstanding_{0};
    int stub_latency_us_ = 0;
};

}

#endif
