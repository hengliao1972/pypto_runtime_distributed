#ifndef LINQU_RUNTIME_DISPATCHER_H
#define LINQU_RUNTIME_DISPATCHER_H

#include "core/coordinate.h"
#include "runtime/linqu_orchestration_api.h"
#include <cstdint>
#include <string>
#include <functional>

namespace linqu {

enum class TaskStatus : uint8_t {
    PENDING    = 0,
    RUNNING    = 1,
    COMPLETED  = 2,
    FAILED     = 3,
};

struct DispatchResult {
    int32_t     task_id;
    TaskStatus  status;
    int32_t     error_code = 0;
};

using DispatchCompletionFn = std::function<void(const DispatchResult&)>;

class LinquDispatcher {
public:
    virtual ~LinquDispatcher() = default;

    virtual bool dispatch(int32_t task_id,
                          const LinquCoordinate& target,
                          const std::string& kernel_so,
                          LinquParam* params, int num_params) = 0;

    virtual void wait_all() = 0;

    virtual void set_completion_callback(DispatchCompletionFn fn) {
        completion_fn_ = std::move(fn);
    }

protected:
    void notify_completion(const DispatchResult& r) {
        if (completion_fn_) completion_fn_(r);
    }
    DispatchCompletionFn completion_fn_;
};

}

#endif
