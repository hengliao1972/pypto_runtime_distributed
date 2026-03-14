#ifndef LINQU_CORE_TENSOR_H
#define LINQU_CORE_TENSOR_H

#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>

namespace linqu {

using TensorHandle = uint64_t;
static constexpr TensorHandle INVALID_HANDLE = 0;

TensorHandle alloc_tensor_handle();

struct LinquTensor {
    TensorHandle handle{INVALID_HANDLE};
    size_t       count{0};

    // Indirect data pointer: shared across all copies of this tensor so
    // that submit_worker() can set the actual HeapRing pointer AFTER the
    // worker lambda has already captured this tensor by value.
    std::shared_ptr<uint64_t*> data_ref;

    // Convenience accessor for the actual data pointer.
    uint64_t* data_ptr() const { return data_ref ? *data_ref : nullptr; }

    // Cross-level readiness signal.  Workers at level L set this after
    // writing data; schedulers at level L+1 observe it to resolve
    // cross-level DAG edges.  Within-level deps are resolved via the
    // event-driven fanin/fanout refcount mechanism (no polling needed).
    std::shared_ptr<std::atomic<bool>> ready;

    bool is_valid()  const { return handle != INVALID_HANDLE; }
    bool is_ready()  const { return ready && ready->load(std::memory_order_acquire); }
    void mark_ready()      { if (ready) ready->store(true, std::memory_order_release); }

    uint64_t scalar() const {
        assert(is_ready() && data_ptr());
        return data_ptr()[0];
    }
};

}

#endif
