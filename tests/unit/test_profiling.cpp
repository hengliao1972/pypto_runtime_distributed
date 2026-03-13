#include "profiling/ring_metrics.h"
#include "runtime/linqu_orchestrator_state.h"
#include <cassert>
#include <cstdio>
#include <cstring>

int main() {
    using namespace linqu;

    printf("=== test_profiling ===\n");

    // Test 1: ProfileReport JSON serialization
    {
        ProfileReport rpt;
        rpt.node_id = "test_node_0";
        rpt.level = 3;

        RingMetrics m;
        m.level = 3;
        m.depth = 0;
        m.task_ring_capacity = 64;
        m.task_ring_peak_used = 8;
        m.task_alloc_count = 16;
        m.scope_begin_count = 3;
        m.scope_end_count = 3;
        rpt.metrics.push_back(m);

        RingSnapshot s;
        s.level = 3;
        s.depth = 0;
        s.label = "after_scope_exit";
        s.timestamp_us = 12345678;
        s.task_capacity = 64;
        s.task_used = 0;
        rpt.snapshots.push_back(s);

        std::string json = rpt.to_json();
        assert(json.find("\"test_node_0\"") != std::string::npos);
        assert(json.find("\"HOST\"") != std::string::npos);
        assert(json.find("\"task_ring_capacity\": 64") != std::string::npos);
        assert(json.find("\"after_scope_exit\"") != std::string::npos);
        printf("  [PASS] Test 1: ProfileReport JSON serialization\n");
    }

    // Test 2: LinquOrchestratorState profiling integration
    {
        LinquOrchestratorState state;
        LinquOrchConfig_Internal cfg;
        cfg.task_ring_window = 32;
        cfg.heap_ring_capacity = 16384;
        LinquCoordinate coord;
        coord.l3_idx = 5;
        state.init(Level::HOST, coord, cfg);

        LinquCoordinate_C self_c = state.self_coord();

        state.scope_begin();
        for (int i = 0; i < 4; i++) {
            uint64_t h = state.alloc_tensor(self_c, 256);
            (void)h;
        }
        state.take_snapshot("after_4_allocs");
        state.scope_end();
        state.take_snapshot("after_scope_exit");

        auto metrics = state.current_metrics();
        assert(metrics.level == 3);
        assert(metrics.task_ring_capacity == 32);
        assert(metrics.buffer_ring_capacity == 16384);
        assert(metrics.scope_begin_count == 1);
        assert(metrics.scope_end_count == 1);
        printf("  [PASS] Test 2: OrchestratorState profiling metrics\n");

        auto& snaps = state.snapshots();
        assert(snaps.size() == 2);
        assert(snaps[0].label == "after_4_allocs");
        assert(snaps[1].label == "after_scope_exit");
        assert(snaps[0].timestamp_us > 0);
        printf("  [PASS] Test 3: OrchestratorState snapshots\n");

        auto profile = state.generate_profile("node_l3_5");
        std::string json = profile.to_json();
        assert(json.find("\"node_l3_5\"") != std::string::npos);
        assert(json.find("\"after_4_allocs\"") != std::string::npos);
        assert(json.find("\"after_scope_exit\"") != std::string::npos);
        printf("  [PASS] Test 4: generate_profile JSON output\n");
    }

    // Test 3: free_tensor counting
    {
        LinquOrchestratorState state;
        LinquOrchConfig_Internal cfg;
        LinquCoordinate coord;
        state.init(Level::HOST, coord, cfg);

        state.free_tensor(1);
        state.free_tensor(2);
        state.free_tensor(3);

        auto metrics = state.current_metrics();
        assert(metrics.free_tensor_count == 3);
        printf("  [PASS] Test 5: free_tensor counting\n");
    }

    printf("=== all profiling tests passed ===\n");
    return 0;
}
