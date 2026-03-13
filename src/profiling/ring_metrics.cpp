#include "profiling/ring_metrics.h"
#include <sstream>

namespace linqu {

static const char* level_label(uint8_t lv) {
    switch (lv) {
        case 0: return "CORE";
        case 1: return "CHIP_DIE";
        case 2: return "CHIP";
        case 3: return "HOST";
        case 4: return "POD";
        case 5: return "CLOS1";
        case 6: return "CLOS2";
        default: return "UNKNOWN";
    }
}

std::string ProfileReport::to_json() const {
    std::ostringstream os;
    os << "{\n";
    os << "  \"node_id\": \"" << node_id << "\",\n";
    os << "  \"level\": \"" << level_label(level) << "\",\n";

    os << "  \"metrics\": [\n";
    for (size_t i = 0; i < metrics.size(); i++) {
        auto& m = metrics[i];
        os << "    {\n";
        os << "      \"level\": \"" << level_label(m.level) << "\",\n";
        os << "      \"depth\": " << m.depth << ",\n";
        os << "      \"task_ring_capacity\": " << m.task_ring_capacity << ",\n";
        os << "      \"task_ring_peak_used\": " << m.task_ring_peak_used << ",\n";
        os << "      \"task_alloc_count\": " << m.task_alloc_count << ",\n";
        os << "      \"task_retire_count\": " << m.task_retire_count << ",\n";
        os << "      \"task_block_count\": " << m.task_block_count << ",\n";
        os << "      \"buffer_ring_capacity\": " << m.buffer_ring_capacity << ",\n";
        os << "      \"buffer_ring_peak_used\": " << m.buffer_ring_peak_used << ",\n";
        os << "      \"buffer_alloc_count\": " << m.buffer_alloc_count << ",\n";
        os << "      \"buffer_alloc_bytes\": " << m.buffer_alloc_bytes << ",\n";
        os << "      \"buffer_retire_bytes\": " << m.buffer_retire_bytes << ",\n";
        os << "      \"scope_begin_count\": " << m.scope_begin_count << ",\n";
        os << "      \"scope_end_count\": " << m.scope_end_count << ",\n";
        os << "      \"free_tensor_count\": " << m.free_tensor_count << "\n";
        os << "    }" << (i + 1 < metrics.size() ? "," : "") << "\n";
    }
    os << "  ],\n";

    os << "  \"snapshots\": [\n";
    for (size_t i = 0; i < snapshots.size(); i++) {
        auto& s = snapshots[i];
        os << "    {\n";
        os << "      \"label\": \"" << s.label << "\",\n";
        os << "      \"level\": \"" << level_label(s.level) << "\",\n";
        os << "      \"depth\": " << s.depth << ",\n";
        os << "      \"timestamp_us\": " << s.timestamp_us << ",\n";
        os << "      \"task_capacity\": " << s.task_capacity << ",\n";
        os << "      \"task_used\": " << s.task_used << ",\n";
        os << "      \"buffer_capacity\": " << s.buffer_capacity << ",\n";
        os << "      \"buffer_used\": " << s.buffer_used << ",\n";
        os << "      \"alloc_count\": " << s.alloc_count << ",\n";
        os << "      \"retire_count\": " << s.retire_count << ",\n";
        os << "      \"free_count\": " << s.free_count << "\n";
        os << "    }" << (i + 1 < snapshots.size() ? "," : "") << "\n";
    }
    os << "  ]\n";
    os << "}\n";
    return os.str();
}

}
