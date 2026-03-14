"""
test_dfs_sum_hierarchy_pl_at.py — DFS Hierarchical Sum Test (pl.at block style)

PyPTO equivalent of test_dfs_sum_hierarchy.cpp using the
    with pl.at(level=..., role=...)
block grammar. The entire L7→L3 hierarchical computation is expressed as a
single nested structure of parallel loops, where each hierarchy level is
annotated with pl.at(level=..., role=...). Worker logic is embedded inline
within pl.at(role=WORKER) blocks.

Normal Python helper functions (read_dfs_data, element_wise_sum) factor out
duplicated compute logic that is called from multiple nesting depths.

Grammar equivalence:
    with pl.at(level=L, role=R):   ←→   @pl.function(level=L, role=R)
        <body>                            def anonymous_fn(...): <body>

Grammar reference: machine_hierarchy_and_function_hierarchy.md §5.5, §5.7, §5.8

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
# Normal Python helper functions — no pl annotations.
# Called from inside pl.at blocks to avoid duplicating compute logic.
# ===========================================================================

def read_dfs_data(path: str, out: Tensor):
    """Read integers from a DFS file into a tensor buffer."""
    with open(path) as f:
        for k in range(out.count):
            out[k] = int(f.readline())


def element_wise_sum(a: Tensor, b: Tensor, out: Tensor):
    """Element-wise sum of two equal-length tensors."""
    for k in range(out.count):
        out[k] = a[k] + b[k]


# ===========================================================================
# main — single nested orchestration expressing the full L7→L3 hierarchy.
#
# Structure:
#   L7 orchestrator
#     └─ for l6: parallel L6 orchestrators
#          └─ for l5: parallel L5 orchestrators
#               └─ for l4: parallel L4 orchestrators
#                    └─ for l3: L3 worker (leaf reader)
#                    └─ tree_reduce at L4
#               └─ tree_reduce at L5
#          └─ tree_reduce at L6
#     └─ tree_reduce at L7
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

    # ─── L7: Global Coordinator ─────────────────────────────────────────
    with pl.at(level=Level.GLOBAL, role=Role.ORCHESTRATOR):

        l6_futures = []
        for l6 in range(NUM_L6):

            # Each L6 instance runs as a parallel orchestrator job.
            def l6_orch(l6=l6):

                # ─── L6: Cluster-lv2 ────────────────────────────────
                with pl.at(level=Level.CLOS2, role=Role.ORCHESTRATOR):

                    l5_futures = []
                    for l5 in range(NUM_L5_PER_L6):

                        def l5_orch(l5=l5):

                            # ─── L5: Supernode ──────────────────────
                            with pl.at(level=Level.CLOS1, role=Role.ORCHESTRATOR):

                                l4_futures = []
                                for l4 in range(NUM_L4_PER_L5):

                                    def l4_orch(l4=l4):

                                        # ─── L4: Pod ────────────────
                                        with pl.at(level=Level.POD, role=Role.ORCHESTRATOR):

                                            # Submit L3 leaf workers
                                            l3_outs = []
                                            for l3 in range(NUM_L3_PER_L4):
                                                out = rt_l3.make_tensor(NUMS_PER_FILE)

                                                # ─── L3: Host worker (leaf) ─
                                                with pl.at(level=Level.HOST,
                                                           role=Role.WORKER,
                                                           inputs=[], outputs=[out]):
                                                    read_dfs_data(
                                                        l3_data_path(l6, l5, l4, l3),
                                                        out)

                                                l3_outs.append(out)

                                            # Reduce L3 outputs on L4 workers
                                            with pl.at(level=Level.POD,
                                                       role=Role.WORKER):
                                                reduce_fn = element_wise_sum

                                            return pl.tree_reduce(
                                                rt_l4, l3_outs,
                                                reduce_fn, "pair_sum")

                                    l4_futures.append(
                                        rt_l4.submit_orchestrator(
                                            "l4_orch", l4_orch))

                                l4_outs = [f.get() for f in l4_futures]

                                # Reduce L4 outputs on L5 workers
                                with pl.at(level=Level.CLOS1,
                                           role=Role.WORKER):
                                    reduce_fn = element_wise_sum

                                return pl.tree_reduce(
                                    rt_l5, l4_outs,
                                    reduce_fn, "pair_sum")

                        l5_futures.append(
                            rt_l5.submit_orchestrator(
                                "l5_orch", l5_orch))

                    l5_outs = [f.get() for f in l5_futures]

                    # Reduce L5 outputs on L6 workers
                    with pl.at(level=Level.CLOS2, role=Role.WORKER):
                        reduce_fn = element_wise_sum

                    return pl.tree_reduce(
                        rt_l6, l5_outs,
                        reduce_fn, "pair_sum")

            l6_futures.append(
                rt_l6.submit_orchestrator("l6_orch", l6_orch))

        l6_outs = [f.get() for f in l6_futures]

        # Reduce L6 outputs on L7 workers
        with pl.at(level=Level.GLOBAL, role=Role.WORKER):
            reduce_fn = element_wise_sum

        result = pl.tree_reduce(
            rt_l7, l6_outs,
            reduce_fn, "pair_sum")

    # ─── Verify ─────────────────────────────────────────────────────────
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

    print("=== DFS Hierarchical Sum Test (pl.at block style) PASSED ===")


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
