#include "core/tensor.h"

namespace linqu {

static std::atomic<TensorHandle> g_next_handle{1};

TensorHandle alloc_tensor_handle() {
    return g_next_handle.fetch_add(1, std::memory_order_relaxed);
}

}
