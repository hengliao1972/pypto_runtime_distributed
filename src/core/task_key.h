#ifndef LINQU_CORE_TASK_KEY_H
#define LINQU_CORE_TASK_KEY_H

#include "core/coordinate.h"
#include <cstdint>
#include <functional>
#include <string>

namespace linqu {

struct TaskKey {
    std::string logical_system;
    LinquCoordinate coord;
    uint16_t scope_depth = 0;
    uint32_t task_id = 0;

    bool operator==(const TaskKey& o) const {
        return logical_system == o.logical_system && coord == o.coord
            && scope_depth == o.scope_depth && task_id == o.task_id;
    }

    bool operator<(const TaskKey& o) const {
        if (logical_system != o.logical_system) return logical_system < o.logical_system;
        if (coord != o.coord) return coord < o.coord;
        if (scope_depth != o.scope_depth) return scope_depth < o.scope_depth;
        return task_id < o.task_id;
    }

    std::string to_string() const {
        return logical_system + ":(" + std::to_string(coord.l6_idx) + ","
            + std::to_string(coord.l5_idx) + "," + std::to_string(coord.l4_idx) + ","
            + std::to_string(coord.l3_idx) + "):d" + std::to_string(scope_depth)
            + ":t" + std::to_string(task_id);
    }
};

}

namespace std {

template<>
struct hash<linqu::TaskKey> {
    size_t operator()(const linqu::TaskKey& k) const noexcept {
        size_t h = std::hash<std::string>{}(k.logical_system);
        h ^= std::hash<uint8_t>{}(k.coord.l6_idx);
        h ^= std::hash<uint8_t>{}(k.coord.l5_idx);
        h ^= std::hash<uint8_t>{}(k.coord.l4_idx);
        h ^= std::hash<uint16_t>{}(k.coord.l3_idx);
        h ^= std::hash<uint16_t>{}(k.coord.l2_idx);
        h ^= std::hash<uint8_t>{}(k.coord.l1_idx);
        h ^= std::hash<uint16_t>{}(k.coord.l0_idx);
        h ^= std::hash<uint16_t>{}(k.scope_depth);
        h ^= std::hash<uint32_t>{}(k.task_id);
        return h;
    }
};

}

#endif
