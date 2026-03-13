#ifndef LINQU_CORE_COORDINATE_H
#define LINQU_CORE_COORDINATE_H

#include "core/level.h"
#include <cstdint>
#include <cstdlib>
#include <string>

namespace linqu {

struct LinquCoordinate {
    uint8_t l6_idx = 0;
    uint8_t l5_idx = 0;
    uint8_t l4_idx = 0;
    uint16_t l3_idx = 0;
    uint16_t l2_idx = 0;
    uint8_t l1_idx = 0;
    uint16_t l0_idx = 0;

    bool operator==(const LinquCoordinate& o) const {
        return l6_idx == o.l6_idx && l5_idx == o.l5_idx && l4_idx == o.l4_idx
            && l3_idx == o.l3_idx && l2_idx == o.l2_idx && l1_idx == o.l1_idx
            && l0_idx == o.l0_idx;
    }

    bool operator!=(const LinquCoordinate& o) const {
        return !(*this == o);
    }

    bool operator<(const LinquCoordinate& o) const {
        if (l6_idx != o.l6_idx) return l6_idx < o.l6_idx;
        if (l5_idx != o.l5_idx) return l5_idx < o.l5_idx;
        if (l4_idx != o.l4_idx) return l4_idx < o.l4_idx;
        if (l3_idx != o.l3_idx) return l3_idx < o.l3_idx;
        if (l2_idx != o.l2_idx) return l2_idx < o.l2_idx;
        if (l1_idx != o.l1_idx) return l1_idx < o.l1_idx;
        return l0_idx < o.l0_idx;
    }

    std::string to_string() const {
        std::string s("(l6=");
        s += std::to_string(l6_idx);
        s += ",l5="; s += std::to_string(l5_idx);
        s += ",l4="; s += std::to_string(l4_idx);
        s += ",l3="; s += std::to_string(l3_idx);
        s += ",l2="; s += std::to_string(l2_idx);
        s += ",l1="; s += std::to_string(l1_idx);
        s += ",l0="; s += std::to_string(l0_idx);
        s += ")";
        return s;
    }

    std::string to_path() const {
        std::string s("L6_");
        s += std::to_string(l6_idx);
        s += "/L5_"; s += std::to_string(l5_idx);
        s += "/L4_"; s += std::to_string(l4_idx);
        s += "/L3_"; s += std::to_string(l3_idx);
        s += "/L2_"; s += std::to_string(l2_idx);
        s += "/L1_"; s += std::to_string(l1_idx);
        s += "/L0_"; s += std::to_string(l0_idx);
        return s;
    }

    uint8_t index_at(Level level) const {
        switch (level_value(level)) {
            case 0: return static_cast<uint8_t>(l0_idx);
            case 1: return l1_idx;
            case 2: return static_cast<uint8_t>(l2_idx);
            case 3: return static_cast<uint8_t>(l3_idx);
            case 4: return l4_idx;
            case 5: return l5_idx;
            case 6: return l6_idx;
            default: return 0;
        }
    }

    static LinquCoordinate from_env() {
        LinquCoordinate c;
        const char* v;
        if ((v = std::getenv("LINQU_L0"))) c.l0_idx = static_cast<uint16_t>(std::atoi(v));
        if ((v = std::getenv("LINQU_L1"))) c.l1_idx = static_cast<uint8_t>(std::atoi(v));
        if ((v = std::getenv("LINQU_L2"))) c.l2_idx = static_cast<uint16_t>(std::atoi(v));
        if ((v = std::getenv("LINQU_L3"))) c.l3_idx = static_cast<uint16_t>(std::atoi(v));
        if ((v = std::getenv("LINQU_L4"))) c.l4_idx = static_cast<uint8_t>(std::atoi(v));
        if ((v = std::getenv("LINQU_L5"))) c.l5_idx = static_cast<uint8_t>(std::atoi(v));
        if ((v = std::getenv("LINQU_L6"))) c.l6_idx = static_cast<uint8_t>(std::atoi(v));
        return c;
    }
};

}

#endif
