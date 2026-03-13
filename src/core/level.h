#ifndef LINQU_CORE_LEVEL_H
#define LINQU_CORE_LEVEL_H

#include <cstdint>

namespace linqu {

enum class Level : uint8_t {
    CORE = 0,
    AIV = 0,
    AIC = 0,
    CORE_GROUP = 0,
    CHIP_DIE = 1,
    L2CACHE = 1,
    CHIP = 2,
    PROCESSOR = 2,
    UMA = 2,
    HOST = 3,
    NODE = 3,
    CLUSTER_0 = 4,
    POD = 4,
    CLUSTER_1 = 5,
    CLOS1 = 5,
    CLUSTER_2 = 6,
    CLOS2 = 6,
};

constexpr int NUM_LEVELS = 7;

constexpr const char* level_name(Level l) {
    switch (static_cast<uint8_t>(l)) {
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

constexpr uint8_t level_value(Level l) {
    return static_cast<uint8_t>(l);
}

}

#endif
