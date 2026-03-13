#ifndef LINQU_RUNTIME_MOCK_DISPATCHER_H
#define LINQU_RUNTIME_MOCK_DISPATCHER_H

#include "runtime/linqu_dispatcher.h"
#include <vector>
#include <mutex>

namespace linqu {

struct MockDispatchRecord {
    int32_t     task_id;
    LinquCoordinate target;
    std::string kernel_so;
    int         num_params;
};

class MockDispatcher : public LinquDispatcher {
public:
    bool dispatch(int32_t task_id,
                  const LinquCoordinate& target,
                  const std::string& kernel_so,
                  LinquParam* params, int num_params) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            records_.push_back({task_id, target, kernel_so, num_params});
        }
        DispatchResult r;
        r.task_id = task_id;
        r.status = TaskStatus::COMPLETED;
        notify_completion(r);
        return true;
    }

    void wait_all() override {}

    std::vector<MockDispatchRecord> records() const {
        std::lock_guard<std::mutex> lk(mu_);
        return records_;
    }

    void reset() {
        std::lock_guard<std::mutex> lk(mu_);
        records_.clear();
    }

private:
    mutable std::mutex mu_;
    std::vector<MockDispatchRecord> records_;
};

}

#endif
