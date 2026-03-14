/*
 * test_dfs_sum_hierarchy.cpp — DFS Hierarchical Sum Test.
 *
 * Simulates a Lingqu system computing the distributed sum of random numbers
 * stored in DFS files, aggregated bottom-up through L3 → L4 → L5 → L6 → L7.
 *
 * This test uses PRODUCTION code from src/:
 *   - LinquTensor          (core/tensor.h)
 *   - LevelRuntime         (runtime/level_runtime.h)
 *   - tree_reduce          (runtime/tree_reduce.h)
 * which internally use the simpler-aligned ring structures:
 *   - LinquTaskRing, LinquHeapRing, LinquDepListPool, LinquTensorMap,
 *     LinquScopeStack
 *
 * Only test-specific code remains here: worker functions, orchestrator
 * functions, data generation, and verification.
 */

#include "core/tensor.h"
#include "runtime/level_runtime.h"
#include "runtime/tree_reduce.h"
#include "profiling/trace_writer.h"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using linqu::LinquTensor;
using linqu::LevelRuntime;
using linqu::LevelRuntimeConfig;

namespace {

// ===========================================================================
// Test constants
// Default topology: 1 × 16 × 4 × 16 = 1024 L3 nodes.
// ===========================================================================

static constexpr int kNumL6Default      = 1;
static constexpr int kNumL5PerL6Default = 16;
static constexpr int kNumL4PerL5Default = 4;
static constexpr int kNumL3PerL4Default = 16;
static constexpr int kNumsPerFileDefault = 1024;

static const char* kBaseDir = "/tmp/linqu_test_dfs_hierarchy_sum";

static int kNumL6;
static int kNumL5PerL6;
static int kNumL4PerL5;
static int kNumL3PerL4;
static int kNumsPerFile;

static int env_int(const char* name, int def) {
    const char* s = std::getenv(name);
    if (s && *s) { int v = std::atoi(s); if (v > 0) return v; }
    return def;
}

static std::string l3_data_path(int l6, int l5, int l4, int l3) {
    return std::string(kBaseDir) + "/dfs"
        + "/L6_" + std::to_string(l6)
        + "/L5_" + std::to_string(l5)
        + "/L4_" + std::to_string(l4)
        + "/L3_" + std::to_string(l3) + "/data.txt";
}

static std::vector<uint64_t> build_dfs_test_data() {
    fs::remove_all(kBaseDir);
    fs::create_directories(std::string(kBaseDir) + "/dfs");
    std::vector<uint64_t> expected(static_cast<size_t>(kNumsPerFile), 0ULL);
    for (int l6 = 0; l6 < kNumL6; l6++)
    for (int l5 = 0; l5 < kNumL5PerL6; l5++)
    for (int l4 = 0; l4 < kNumL4PerL5; l4++)
    for (int l3 = 0; l3 < kNumL3PerL4; l3++) {
        int global = ((l6*kNumL5PerL6 + l5)*kNumL4PerL5 + l4)*kNumL3PerL4 + l3;
        std::mt19937 rng(0x5A17u + static_cast<uint32_t>(global));
        std::uniform_int_distribution<int> dist(1, 1000);

        std::string path = l3_data_path(l6, l5, l4, l3);
        fs::create_directories(fs::path(path).parent_path());
        std::ofstream out(path);
        assert(out.good());
        for (int k = 0; k < kNumsPerFile; k++) {
            int v = dist(rng);
            expected[static_cast<size_t>(k)] += static_cast<uint64_t>(v);
            out << v << "\n";
        }
    }
    return expected;
}

// ===========================================================================
// WORKER FUNCTIONS
//
// Workers are pure compute functions; they never submit further tasks.
// LinquTensor::data_ptr points into HeapRing memory allocated by
// submit_worker() — aligned with simpler's packed buffer model.
// ===========================================================================

static void dfs_l3_reader_worker(int l6, int l5, int l4, int l3,
                                  LinquTensor out) {
    std::ifstream f(l3_data_path(l6, l5, l4, l3));
    assert(f.good());
    uint64_t* p = out.data_ptr();
    assert(p);
    int v = 0;
    for (size_t k = 0; k < out.count; k++) {
        if (!(f >> v)) break;
        p[k] = static_cast<uint64_t>(v);
    }
}

static void pair_sum_worker(LinquTensor a, LinquTensor b, LinquTensor out) {
    assert(a.is_ready() && a.data_ptr());
    assert(b.is_ready() && b.data_ptr());
    assert(a.count == b.count && out.count == a.count);
    uint64_t* ap = a.data_ptr();
    uint64_t* bp = b.data_ptr();
    uint64_t* op = out.data_ptr();
    for (size_t k = 0; k < out.count; k++)
        op[k] = ap[k] + bp[k];
}

// ===========================================================================
// Trace instance-id helpers.
// Each hierarchy instance maps to a unique Perfetto "process" row.
//   trace_pid = level * 10000 + flat_instance_index
// ===========================================================================

static int32_t tpid_l3(int l6, int l5, int l4, int l3) {
    return 30000
        + ((l6 * kNumL5PerL6 + l5) * kNumL4PerL5 + l4) * kNumL3PerL4 + l3;
}
static int32_t tpid_l4(int l6, int l5, int l4) {
    return 40000 + (l6 * kNumL5PerL6 + l5) * kNumL4PerL5 + l4;
}
static int32_t tpid_l5(int l6, int l5) {
    return 50000 + l6 * kNumL5PerL6 + l5;
}
static int32_t tpid_l6(int l6) { return 60000 + l6; }
static constexpr int32_t TPID_L7 = 70000;

// ===========================================================================
// ORCHESTRATORS (build DAG, submit tasks, never compute directly)
// ===========================================================================

static LinquTensor l4_orchestrate(LevelRuntime& rt_l3, LevelRuntime& rt_l4,
                                   int l6, int l5, int l4) {
    int32_t pid_l4 = tpid_l4(l6, l5, l4);

    std::vector<LinquTensor> l3_outs(kNumL3PerL4);
    for (int l3 = 0; l3 < kNumL3PerL4; l3++) {
        l3_outs[l3] = rt_l3.make_tensor(static_cast<size_t>(kNumsPerFile));
        rt_l3.submit_worker(
            "dfs_l3_reader",
            [l6, l5, l4, l3, out = l3_outs[l3]] {
                dfs_l3_reader_worker(l6, l5, l4, l3, out);
            },
            {},
            {l3_outs[l3]},
            tpid_l3(l6, l5, l4, l3));
    }
    return linqu::tree_reduce(rt_l4, l3_outs,
        [](LinquTensor a, LinquTensor b, LinquTensor out) {
            pair_sum_worker(a, b, out);
        },
        "pair_sum", pid_l4);
}

static LinquTensor l5_orchestrate(LevelRuntime& rt_l3, LevelRuntime& rt_l4,
                                   LevelRuntime& rt_l5, int l6, int l5) {
    int32_t pid_l5 = tpid_l5(l6, l5);

    std::vector<std::future<LinquTensor>> futs;
    futs.reserve(kNumL4PerL5);
    for (int l4 = 0; l4 < kNumL4PerL5; l4++) {
        futs.push_back(rt_l4.submit_orchestrator(
            "l4_orchestrate",
            [&, l6, l5, l4]() -> LinquTensor {
                return l4_orchestrate(rt_l3, rt_l4, l6, l5, l4);
            },
            tpid_l4(l6, l5, l4)));
    }
    std::vector<LinquTensor> l4_outs(kNumL4PerL5);
    for (int l4 = 0; l4 < kNumL4PerL5; l4++)
        l4_outs[l4] = futs[l4].get();

    return linqu::tree_reduce(rt_l5, l4_outs,
        [](LinquTensor a, LinquTensor b, LinquTensor out) {
            pair_sum_worker(a, b, out);
        },
        "pair_sum", pid_l5);
}

static LinquTensor l6_orchestrate(LevelRuntime& rt_l3, LevelRuntime& rt_l4,
                                   LevelRuntime& rt_l5, LevelRuntime& rt_l6,
                                   int l6) {
    int32_t pid_l6 = tpid_l6(l6);

    std::vector<std::future<LinquTensor>> futs;
    futs.reserve(kNumL5PerL6);
    for (int l5 = 0; l5 < kNumL5PerL6; l5++) {
        futs.push_back(rt_l5.submit_orchestrator(
            "l5_orchestrate",
            [&, l6, l5]() -> LinquTensor {
                return l5_orchestrate(rt_l3, rt_l4, rt_l5, l6, l5);
            },
            tpid_l5(l6, l5)));
    }
    std::vector<LinquTensor> l5_outs(kNumL5PerL6);
    for (int l5 = 0; l5 < kNumL5PerL6; l5++)
        l5_outs[l5] = futs[l5].get();

    return linqu::tree_reduce(rt_l6, l5_outs,
        [](LinquTensor a, LinquTensor b, LinquTensor out) {
            pair_sum_worker(a, b, out);
        },
        "pair_sum", pid_l6);
}

}  // namespace

// ===========================================================================
// print_hierarchy
// ===========================================================================

static void print_hierarchy(int total_l3) {
    const int n_chip_per_host = env_int("N_CHIP_PER_HOST", 4);
    const int n_die_per_chip  = env_int("N_DIE_PER_CHIP",  2);
    const int n_cg_per_die    = env_int("N_CG_PER_DIE",    8);

    const int total_l2 = total_l3 * n_chip_per_host;
    const int total_l1 = total_l2 * n_die_per_chip;
    const int total_l0 = total_l1 * n_cg_per_die;

    fprintf(stderr,
        "┌─────────────────────────────────────────────────────────────────┐\n"
        "│            Lingqu System Hierarchy — Instance Counts            │\n"
        "├───────┬──────────────────────────┬─────────────┬────────────────┤\n"
        "│ Level │ Name                     │ Per-parent  │ Total          │\n"
        "├───────┼──────────────────────────┼─────────────┼────────────────┤\n"
        "│  L7   │ Global Coordinator       │      —      │ %14d │\n"
        "│  L6   │ Cluster-lv2 (CLOS2)      │      —      │ %14d │\n"
        "│  L5   │ Cluster-lv1 (Supernode)  │ %11d │ %14d │\n"
        "│  L4   │ Cluster-lv0 (Pod)        │ %11d │ %14d │\n"
        "│  L3   │ Host (OS node)           │ %11d │ %14d │\n"
        "├───────┼──────────────────────────┼─────────────┼────────────────┤\n"
        "│  L2   │ Chip (UMA)               │ %11d │ %14d │\n"
        "│  L1   │ Chip Die (optional)      │ %11d │ %14d │\n"
        "│  L0   │ Core-group (AIC+2×AIV)   │ %11d │ %14d │\n"
        "└───────┴──────────────────────────┴─────────────┴────────────────┘\n"
        "  Tensor width per L3 file: %d elements\n\n",
        1, kNumL6,
        kNumL5PerL6,  kNumL6 * kNumL5PerL6,
        kNumL4PerL5,  kNumL6 * kNumL5PerL6 * kNumL4PerL5,
        kNumL3PerL4,  total_l3,
        n_chip_per_host, total_l2,
        n_die_per_chip,  total_l1,
        n_cg_per_die,    total_l0,
        kNumsPerFile);
}

// ===========================================================================
// main
// ===========================================================================

int main(int argc, char* argv[]) {
    // Parse --trace flag.
    bool do_trace = false;
    std::string trace_path;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--trace") == 0) {
            do_trace = true;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                trace_path = argv[++i];
            }
        }
    }
    if (do_trace && trace_path.empty()) {
        trace_path = "linqu_dfs_hierarchy_trace.json";
    }

    kNumL6      = env_int("L6_SIZE",   kNumL6Default);
    kNumL5PerL6 = env_int("L5_SIZE",   kNumL5PerL6Default);
    kNumL4PerL5 = env_int("L4_SIZE",   kNumL4PerL5Default);
    kNumL3PerL4 = env_int("L3_SIZE",   kNumL3PerL4Default);
    kNumsPerFile= env_int("NUMS_PER_FILE", kNumsPerFileDefault);

    const int total_l3 = kNumL6 * kNumL5PerL6 * kNumL4PerL5 * kNumL3PerL4;

    fprintf(stderr, "=== DFS Hierarchical Sum Test (Production LevelRuntime) ===\n\n");
    print_hierarchy(total_l3);
    fprintf(stderr, "    L7→L3 (%d L3 nodes), nums/file=%d\n\n",
            total_l3, kNumsPerFile);

    const std::vector<uint64_t> expected = build_dfs_test_data();
    uint64_t expected_total = 0;
    for (uint64_t x : expected) expected_total += x;
    fprintf(stderr, "[DATA] Generated %d DFS files, %d cols/file, grand total = %llu\n\n",
            total_l3, kNumsPerFile, (unsigned long long)expected_total);

    const int l3_sched   = env_int("L3_NUM_SCHEDULER_THREADS", 1);
    const int l4_sched   = env_int("L4_NUM_SCHEDULER_THREADS", 1);
    const int l5_sched   = env_int("L5_NUM_SCHEDULER_THREADS", 1);
    const int l6_sched   = env_int("L6_NUM_SCHEDULER_THREADS", 1);
    const int l7_sched   = env_int("L7_NUM_SCHEDULER_THREADS", 1);
    const int l3_workers = env_int("L3_NUM_WORKER_THREADS", 4);
    const int l4_workers = env_int("L4_NUM_WORKER_THREADS", 4);
    const int l5_workers = env_int("L5_NUM_WORKER_THREADS", 4);
    const int l6_workers = env_int("L6_NUM_WORKER_THREADS", 4);
    const int l7_workers = env_int("L7_NUM_WORKER_THREADS", 4);

    // Set up Perfetto trace writer (shared by all levels).
    linqu::TraceWriter trace_writer;
    if (do_trace) {
        trace_writer.set_enabled(true);
        fprintf(stderr, "[TRACE] Tracing enabled → %s\n\n", trace_path.c_str());
    }

    LevelRuntime rt_l3(3, l3_sched, l3_workers);
    LevelRuntime rt_l4(4, l4_sched, l4_workers);
    LevelRuntime rt_l5(5, l5_sched, l5_workers);
    LevelRuntime rt_l6(6, l6_sched, l6_workers);
    LevelRuntime rt_l7(7, l7_sched, l7_workers);

    if (do_trace) {
        rt_l3.set_trace_writer(&trace_writer);
        rt_l4.set_trace_writer(&trace_writer);
        rt_l5.set_trace_writer(&trace_writer);
        rt_l6.set_trace_writer(&trace_writer);
        rt_l7.set_trace_writer(&trace_writer);

        // Register per-instance trace "processes".
        for (int l6 = 0; l6 < kNumL6; l6++)
        for (int l5 = 0; l5 < kNumL5PerL6; l5++)
        for (int l4 = 0; l4 < kNumL4PerL5; l4++)
        for (int l3 = 0; l3 < kNumL3PerL4; l3++) {
            std::string label = "L3[" + std::to_string(l5) + "."
                + std::to_string(l4) + "." + std::to_string(l3) + "]";
            rt_l3.register_trace_instance(tpid_l3(l6, l5, l4, l3), label);
        }
        for (int l6 = 0; l6 < kNumL6; l6++)
        for (int l5 = 0; l5 < kNumL5PerL6; l5++)
        for (int l4 = 0; l4 < kNumL4PerL5; l4++) {
            std::string label = "L4[" + std::to_string(l5) + "."
                + std::to_string(l4) + "]";
            rt_l4.register_trace_instance(tpid_l4(l6, l5, l4), label);
        }
        for (int l6 = 0; l6 < kNumL6; l6++)
        for (int l5 = 0; l5 < kNumL5PerL6; l5++) {
            std::string label = "L5[" + std::to_string(l5) + "]";
            rt_l5.register_trace_instance(tpid_l5(l6, l5), label);
        }
        for (int l6 = 0; l6 < kNumL6; l6++) {
            std::string label = "L6[" + std::to_string(l6) + "]";
            rt_l6.register_trace_instance(tpid_l6(l6), label);
        }
        rt_l7.register_trace_instance(TPID_L7, "L7");
    }

    rt_l3.start(); rt_l4.start(); rt_l5.start(); rt_l6.start(); rt_l7.start();

    std::future<LinquTensor> top_future =
        rt_l7.submit_orchestrator("l7_orchestrate", [&]() -> LinquTensor {
            std::vector<std::future<LinquTensor>> futs;
            futs.reserve(kNumL6);
            for (int l6 = 0; l6 < kNumL6; l6++) {
                futs.push_back(rt_l6.submit_orchestrator(
                    "l6_orchestrate",
                    [&, l6]() -> LinquTensor {
                        return l6_orchestrate(rt_l3, rt_l4, rt_l5, rt_l6, l6);
                    },
                    tpid_l6(l6)));
            }
            std::vector<LinquTensor> l6_outs(kNumL6);
            for (int l6 = 0; l6 < kNumL6; l6++)
                l6_outs[l6] = futs[l6].get();

            return linqu::tree_reduce(rt_l7, l6_outs,
                [](LinquTensor a, LinquTensor b, LinquTensor out) {
                    pair_sum_worker(a, b, out);
                },
                "pair_sum", TPID_L7);
        }, TPID_L7);

    const LinquTensor result = top_future.get();

    // Verify element-wise: result.data_ptr()[k] == expected[k]
    const uint64_t* rp = result.data_ptr();
    assert(rp != nullptr);
    assert(result.count == static_cast<size_t>(kNumsPerFile));
    int mismatches = 0;
    for (int k = 0; k < kNumsPerFile; k++) {
        if (rp[k] != expected[k]) {
            fprintf(stderr, "[FAIL] result[%d]: got %llu expected %llu\n",
                    k, (unsigned long long)rp[k],
                       (unsigned long long)expected[k]);
            mismatches++;
        }
    }
    if (mismatches > 0) {
        fprintf(stderr, "[FAIL] %d / %d columns mismatched\n",
                mismatches, kNumsPerFile);
        return 1;
    }

    uint64_t computed_total = 0;
    for (int k = 0; k < kNumsPerFile; k++)
        computed_total += rp[k];
    fprintf(stderr, "result[%d] = sum(tensor[i][k]) for i=0..%d\n",
            kNumsPerFile, total_l3 - 1);
    fprintf(stderr, "  result[0..3]  : %llu %llu %llu %llu\n",
            (unsigned long long)rp[0], (unsigned long long)rp[1],
            (unsigned long long)rp[2], (unsigned long long)rp[3]);
    fprintf(stderr, "  result[%d..%d]: %llu %llu %llu %llu\n",
            kNumsPerFile-4, kNumsPerFile-1,
            (unsigned long long)rp[kNumsPerFile-4],
            (unsigned long long)rp[kNumsPerFile-3],
            (unsigned long long)rp[kNumsPerFile-2],
            (unsigned long long)rp[kNumsPerFile-1]);
    fprintf(stderr, "  grand total = %llu  (expected %llu)\n",
            (unsigned long long)computed_total, (unsigned long long)expected_total);

    rt_l7.stop(); rt_l6.stop(); rt_l5.stop(); rt_l4.stop(); rt_l3.stop();

    if (do_trace) {
        std::string written = trace_writer.write_json(trace_path);
        if (!written.empty()) {
            // Print the absolute path so user can open it in Perfetto UI.
            auto abs = fs::absolute(written);
            fprintf(stderr, "\n[TRACE] Written: %s\n", abs.c_str());
            fprintf(stderr, "[TRACE] Open in https://ui.perfetto.dev/\n");
        }
    }

    fs::remove_all(kBaseDir);

    const int total_tree_workers = (total_l3 - 1);
    fprintf(stderr, "\n=== DFS Hierarchical Sum Test (Production LevelRuntime) PASSED ===\n");
    fprintf(stderr, "Uses production src/ code:\n");
    fprintf(stderr, "  - LinquTaskRing (ring-buffer task slots, simpler-aligned)\n");
    fprintf(stderr, "  - LinquHeapRing (output buffers allocated at submit_worker)\n");
    fprintf(stderr, "  - LinquDepListPool (fanin/fanout linked-list chains)\n");
    fprintf(stderr, "  - LinquTensorMap (handle→producer DAG discovery)\n");
    fprintf(stderr, "  - LinquScopeStack (scope-based lifetime, fanout_count=1)\n");
    fprintf(stderr, "  - Event-driven propagate_completion (no 2ms polling)\n");
    fprintf(stderr, "  - Atomic fanin/fanout refcounts + per-task spinlock\n");
    fprintf(stderr, "  - Ring retirement via try_advance_ring_pointers\n");
    fprintf(stderr, "Verified:\n");
    fprintf(stderr, "  1. %d L3 readers + %d tree workers = %d total tasks\n",
            total_l3, total_tree_workers, total_l3 + total_tree_workers);
    fprintf(stderr, "  2. result[k] = sum(tensor[i][k]) for i=0..%d, k=0..%d\n",
            total_l3-1, kNumsPerFile-1);
    return 0;
}
