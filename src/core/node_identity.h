#ifndef LINQU_CORE_NODE_IDENTITY_H
#define LINQU_CORE_NODE_IDENTITY_H

#include "core/coordinate.h"
#include <string>
#include <cstdlib>

namespace linqu {

struct NodeIdentity {
    LinquCoordinate coord;
    std::string physical_system_name;
    std::string logical_system_name;

    static NodeIdentity from_env() {
        NodeIdentity id;
        id.coord = LinquCoordinate::from_env();
        const char* phys = std::getenv("LINQU_PHYSICAL_SYSTEM");
        if (phys) id.physical_system_name = phys;
        const char* logic = std::getenv("LINQU_LOGICAL_SYSTEM");
        if (logic) id.logical_system_name = logic;
        return id;
    }

    std::string to_string() const {
        return "NodeIdentity{coord=" + coord.to_string() +
               ", phys=" + physical_system_name +
               ", logic=" + logical_system_name + "}";
    }
};

}

#endif
