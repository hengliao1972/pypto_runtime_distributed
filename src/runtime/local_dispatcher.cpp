#include "runtime/local_dispatcher.h"
#include <thread>
#include <chrono>
#include <cstdio>
#include <dlfcn.h>

namespace linqu {

LocalDispatcher::LocalDispatcher() = default;
LocalDispatcher::~LocalDispatcher() = default;

bool LocalDispatcher::dispatch(int32_t task_id,
                                const LinquCoordinate& target,
                                const std::string& kernel_so,
                                LinquParam* params, int num_params) {
    dispatched_count_.fetch_add(1);
    outstanding_.fetch_add(1);

    if (stub_latency_us_ > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(stub_latency_us_));
    }

    // L3→L2 dispatch is stubbed in Phase 0.
    // Attempt to dlopen the kernel for local (same-process) execution.
    void* handle = dlopen(kernel_so.c_str(), RTLD_NOW);
    if (handle) {
        dlclose(handle);
    }

    outstanding_.fetch_sub(1);

    DispatchResult result;
    result.task_id = task_id;
    result.status = TaskStatus::COMPLETED;
    result.error_code = 0;
    notify_completion(result);

    return true;
}

void LocalDispatcher::wait_all() {
    while (outstanding_.load() > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

}
