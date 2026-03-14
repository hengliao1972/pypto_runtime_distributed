#ifndef LINQU_RUNTIME_TREE_REDUCE_H
#define LINQU_RUNTIME_TREE_REDUCE_H

#include "core/tensor.h"
#include "runtime/level_runtime.h"
#include <cassert>
#include <functional>
#include <vector>

namespace linqu {

// tree_reduce — binary reduction tree DAG builder.
//
// Submits ALL internal-node workers upfront (no waiting between rounds).
// The tensor-map DAG tracks which internal-node task depends on which
// leaf / earlier-round tensor.  The scheduler dispatches each internal-node
// worker automatically the moment both of its input tensors are marked
// ready by their producing workers.
//
// pair_fn(a, b, out): user-supplied function that reduces two input
// tensors into one output tensor (element-wise sum, max, etc.).
//
// Returns the root tensor after waiting for the last worker to complete.

inline LinquTensor tree_reduce(
    LevelRuntime& rt,
    std::vector<LinquTensor> leaves,
    std::function<void(LinquTensor, LinquTensor, LinquTensor)> pair_fn,
    const std::string& pair_name = "pair_reduce",
    int32_t trace_pid = -1)
{
    assert(!leaves.empty());
    if (leaves.size() == 1) return leaves[0];

    std::vector<LinquTensor> current = std::move(leaves);
    std::future<void> root_fut;

    while (current.size() > 1) {
        std::vector<LinquTensor> next;
        next.reserve((current.size() + 1) / 2);

        for (size_t i = 0; i + 1 < current.size(); i += 2) {
            LinquTensor out = rt.make_tensor(current[i].count);

            root_fut = rt.submit_worker(
                pair_name,
                [pair_fn, a = current[i], b = current[i+1], out]() mutable {
                    pair_fn(a, b, out);
                },
                {current[i], current[i+1]},
                {out},
                trace_pid);

            next.push_back(out);
        }

        if (current.size() % 2 == 1)
            next.push_back(current.back());

        current = std::move(next);
    }

    if (root_fut.valid()) root_fut.get();
    return current[0];
}

}

#endif
