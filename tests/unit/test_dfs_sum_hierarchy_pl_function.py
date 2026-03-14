"""
test_dfs_sum_hierarchy_pl_function.py — DFS Hierarchical Sum Test (pl.function style)

PyPTO equivalent of test_dfs_sum_hierarchy.cpp using ONLY the
@pl.function(level=..., role=...) decorator grammar.  Each C++ function maps
to a @pl.function-decorated Python function:

    C++ function              →  Python @pl.function
    ─────────────────────────────────────────────────────────
    dfs_l3_reader_worker      →  dfs_l3_reader_worker   (HOST   WORKER)
    pair_sum_worker           →  pair_sum_worker         (any    WORKER)
    l4_orchestrate            →  l4_orchestrate          (POD    ORCHESTRATOR)
    l5_orchestrate            →  l5_orchestrate          (CLOS1  ORCHESTRATOR)
    l6_orchestrate            →  l6_orchestrate          (CLOS2  ORCHESTRATOR)
    l7_orchestrate            →  l7_orchestrate          (GLOBAL ORCHESTRATOR)

Grammar reference: machine_hierarchy_and_function_hierarchy.md §5.4, §5.7, §5.8

Topology (default): 1 × 16 × 4 × 16 = 1024 L3 nodes, 1024 numbers per file.

    L7  Global Coordinator  ─── 1 instance
    L6  Cluster-lv2 (CLOS2) ─── 1 instance
    L5  Cluster-lv1 (Supernode) ─── 16 instances
    L4  Cluster-lv0 (Pod) ─── 64 instances
    L3  Host (OS node) ─── 1024 instances
"""

import pl
from pl import Tensor, Level, Role, LevelRuntime

# ---------------------------------------------------------------------------
# Topology constants (configurable via environment)
# ---------------------------------------------------------------------------

NUM_L6:        int = pl.env_int("L6_SIZE", 1)
NUM_L5_PER_L6: int = pl.env_int("L5_SIZE", 16)
NUM_L4_PER_L5: int = pl.env_int("L4_SIZE", 4)
NUM_L3_PER_L4: int = pl.env_int("L3_SIZE", 16)
NUMS_PER_FILE: int = pl.env_int("NUMS_PER_FILE", 1024)

BASE_DIR = "/tmp/linqu_test_dfs_hierarchy_sum"


def l3_data_path(l6: int, l5: int, l4: int, l3: int) -> str:
    return f"{BASE_DIR}/dfs/L6_{l6}/L5_{l5}/L4_{l4}/L3_{l3}/data.txt"


# ===========================================================================
# WORKER FUNCTIONS  (@pl.function with role=WORKER)
#
# Workers are pure compute functions; they never submit further tasks.
# Tensor output storage is allocated by the runtime at submit_worker() time,
# matching simpler's packed-buffer protocol (§7.3A-1).
# Only tensor-typed parameters participate in the DAG (§7.3A-2).
#
# Maps to:
#   C++  static void dfs_l3_reader_worker(int l6, ..., LinquTensor out)
#   C++  static void pair_sum_worker(LinquTensor a, LinquTensor b, LinquTensor out)
# ===========================================================================

@pl.function(level=Level.HOST, role=Role.WORKER)
def dfs_l3_reader_worker(l6: int, l5: int, l4: int, l3: int,
                         out: Tensor):
    """Read one DFS file into the output tensor.

    Scalars (l6, l5, l4, l3) are captured via lambda and invisible to the
    scheduler.  Only ``out`` is tracked as a DAG output edge.
    """
    with open(l3_data_path(l6, l5, l4, l3)) as f:
        for k in range(out.count):
            out[k] = int(f.readline())


@pl.function(role=Role.WORKER)
def pair_sum_worker(a: Tensor, b: Tensor, out: Tensor):
    """Element-wise sum of two tensors — used as the tree-reduction kernel."""
    assert a.count == b.count == out.count
    for k in range(out.count):
        out[k] = a[k] + b[k]


# ===========================================================================
# ORCHESTRATOR FUNCTIONS  (@pl.function with role=ORCHESTRATOR)
#
# Orchestrators build the task DAG, submit workers (and child orchestrators),
# and wait on futures.  They never compute data directly.
#
# Maps to:
#   C++  static LinquTensor l4_orchestrate(LevelRuntime& rt_l3, ...)
#   C++  static LinquTensor l5_orchestrate(...)
#   C++  static LinquTensor l6_orchestrate(...)
#   C++  L7 inline lambda inside main()
# ===========================================================================

@pl.function(level=Level.POD, role=Role.ORCHESTRATOR)
def l4_orchestrate(rt_l3: LevelRuntime, rt_l4: LevelRuntime,
                   l6: int, l5: int, l4: int) -> Tensor:
    """L4 orchestrator: submit L3 readers, then tree-reduce their outputs on L4.

    For each child L3 host, submit a dfs_l3_reader_worker.  The resulting
    tensors are reduced via pl.tree_reduce on the L4 runtime, producing a
    single tensor with the element-wise sum across all L3 children.
    """
    l3_outs: list[Tensor] = []
    for l3 in range(NUM_L3_PER_L4):
        out = rt_l3.make_tensor(NUMS_PER_FILE)
        rt_l3.submit_worker(
            name="dfs_l3_reader",
            fn=lambda _l6=l6, _l5=l5, _l4=l4, _l3=l3, _out=out:
                dfs_l3_reader_worker(_l6, _l5, _l4, _l3, _out),
            inputs=[],
            outputs=[out],
        )
        l3_outs.append(out)

    return pl.tree_reduce(
        rt_l4, l3_outs,
        pair_fn=lambda a, b, out: pair_sum_worker(a, b, out),
        name="pair_sum",
    )


@pl.function(level=Level.CLOS1, role=Role.ORCHESTRATOR)
def l5_orchestrate(rt_l3: LevelRuntime, rt_l4: LevelRuntime,
                   rt_l5: LevelRuntime,
                   l6: int, l5: int) -> Tensor:
    """L5 orchestrator: fan out to L4 orchestrators, then tree-reduce on L5."""
    l4_futures = []
    for l4 in range(NUM_L4_PER_L5):
        l4_futures.append(
            rt_l4.submit_orchestrator(
                name="l4_orchestrate",
                fn=lambda _l6=l6, _l5=l5, _l4=l4:
                    l4_orchestrate(rt_l3, rt_l4, _l6, _l5, _l4),
            )
        )

    l4_outs = [f.get() for f in l4_futures]

    return pl.tree_reduce(
        rt_l5, l4_outs,
        pair_fn=lambda a, b, out: pair_sum_worker(a, b, out),
        name="pair_sum",
    )


@pl.function(level=Level.CLOS2, role=Role.ORCHESTRATOR)
def l6_orchestrate(rt_l3: LevelRuntime, rt_l4: LevelRuntime,
                   rt_l5: LevelRuntime, rt_l6: LevelRuntime,
                   l6: int) -> Tensor:
    """L6 orchestrator: fan out to L5 orchestrators, then tree-reduce on L6."""
    l5_futures = []
    for l5 in range(NUM_L5_PER_L6):
        l5_futures.append(
            rt_l5.submit_orchestrator(
                name="l5_orchestrate",
                fn=lambda _l6=l6, _l5=l5:
                    l5_orchestrate(rt_l3, rt_l4, rt_l5, _l6, _l5),
            )
        )

    l5_outs = [f.get() for f in l5_futures]

    return pl.tree_reduce(
        rt_l6, l5_outs,
        pair_fn=lambda a, b, out: pair_sum_worker(a, b, out),
        name="pair_sum",
    )


@pl.function(level=Level.GLOBAL, role=Role.ORCHESTRATOR)
def l7_orchestrate(rt_l3: LevelRuntime, rt_l4: LevelRuntime,
                   rt_l5: LevelRuntime, rt_l6: LevelRuntime,
                   rt_l7: LevelRuntime) -> Tensor:
    """L7 (global) orchestrator: fan out to L6, then tree-reduce on L7."""
    l6_futures = []
    for l6 in range(NUM_L6):
        l6_futures.append(
            rt_l6.submit_orchestrator(
                name="l6_orchestrate",
                fn=lambda _l6=l6:
                    l6_orchestrate(rt_l3, rt_l4, rt_l5, rt_l6, _l6),
            )
        )

    l6_outs = [f.get() for f in l6_futures]

    return pl.tree_reduce(
        rt_l7, l6_outs,
        pair_fn=lambda a, b, out: pair_sum_worker(a, b, out),
        name="pair_sum",
    )


# ===========================================================================
# main — set up runtimes, generate test data, run, verify
#
# Maps to:  C++  int main(int argc, char* argv[])
# ===========================================================================

def main():
    total_l3 = NUM_L6 * NUM_L5_PER_L6 * NUM_L4_PER_L5 * NUM_L3_PER_L4

    expected = build_dfs_test_data(total_l3)

    rt_l3 = LevelRuntime(level=3, num_scheduler_threads=1, num_worker_threads=4)
    rt_l4 = LevelRuntime(level=4, num_scheduler_threads=1, num_worker_threads=4)
    rt_l5 = LevelRuntime(level=5, num_scheduler_threads=1, num_worker_threads=4)
    rt_l6 = LevelRuntime(level=6, num_scheduler_threads=1, num_worker_threads=4)
    rt_l7 = LevelRuntime(level=7, num_scheduler_threads=1, num_worker_threads=4)

    trace_writer = pl.TraceWriter()
    if pl.args.trace:
        trace_writer.set_enabled(True)
        for rt in [rt_l3, rt_l4, rt_l5, rt_l6, rt_l7]:
            rt.set_trace_writer(trace_writer)

    for rt in [rt_l3, rt_l4, rt_l5, rt_l6, rt_l7]:
        rt.start()

    top_future = rt_l7.submit_orchestrator(
        name="l7_orchestrate",
        fn=lambda: l7_orchestrate(rt_l3, rt_l4, rt_l5, rt_l6, rt_l7),
    )

    result: Tensor = top_future.get()

    # --- Verify element-wise ---
    assert result.count == NUMS_PER_FILE
    for k in range(NUMS_PER_FILE):
        assert result[k] == expected[k], (
            f"result[{k}]: got {result[k]} expected {expected[k]}")

    computed_total = sum(result[k] for k in range(NUMS_PER_FILE))
    expected_total = sum(expected)
    print(f"result[{NUMS_PER_FILE}] = sum(tensor[i][k]) for i=0..{total_l3 - 1}")
    print(f"  grand total = {computed_total}  (expected {expected_total})")

    for rt in [rt_l7, rt_l6, rt_l5, rt_l4, rt_l3]:
        rt.stop()

    if pl.args.trace:
        path = trace_writer.write_json(
            pl.args.trace_path or "linqu_dfs_hierarchy_trace.json")
        print(f"[TRACE] Written: {path}")

    print("=== DFS Hierarchical Sum Test (@pl.function style) PASSED ===")


# ===========================================================================
# Data generation helper (same random seed scheme as C++ test)
# ===========================================================================

def build_dfs_test_data(total_l3: int) -> list[int]:
    """Generate DFS files and return the expected element-wise sum vector."""
    import os
    import random
    import shutil

    if os.path.exists(BASE_DIR):
        shutil.rmtree(BASE_DIR)

    expected = [0] * NUMS_PER_FILE

    for l6 in range(NUM_L6):
        for l5 in range(NUM_L5_PER_L6):
            for l4 in range(NUM_L4_PER_L5):
                for l3 in range(NUM_L3_PER_L4):
                    global_idx = (
                        (l6 * NUM_L5_PER_L6 + l5) * NUM_L4_PER_L5 + l4
                    ) * NUM_L3_PER_L4 + l3
                    rng = random.Random(0x5A17 + global_idx)

                    path = l3_data_path(l6, l5, l4, l3)
                    os.makedirs(os.path.dirname(path), exist_ok=True)
                    with open(path, "w") as f:
                        for k in range(NUMS_PER_FILE):
                            v = rng.randint(1, 1000)
                            expected[k] += v
                            f.write(f"{v}\n")

    return expected


if __name__ == "__main__":
    main()
