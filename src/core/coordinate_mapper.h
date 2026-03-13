#ifndef LINQU_CORE_COORDINATE_MAPPER_H
#define LINQU_CORE_COORDINATE_MAPPER_H

#include "core/coordinate.h"
#include <string>
#include <cstdlib>
#include <sstream>

namespace linqu {

class CoordinateMapper {
public:
    virtual ~CoordinateMapper() = default;
    virtual LinquCoordinate map(const std::string& address) const = 0;
};

class OctetMapper : public CoordinateMapper {
public:
    LinquCoordinate map(const std::string& ip) const override {
        LinquCoordinate c;
        unsigned a = 0, b = 0, cc = 0, d = 0;
        if (sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &cc, &d) == 4) {
            c.l5_idx = static_cast<uint8_t>(b);
            c.l4_idx = static_cast<uint8_t>(cc);
            c.l3_idx = static_cast<uint16_t>(d);
        }
        return c;
    }
};

class EnvMapper : public CoordinateMapper {
public:
    LinquCoordinate map(const std::string&) const override {
        return LinquCoordinate::from_env();
    }
};

class FixedMapper : public CoordinateMapper {
public:
    explicit FixedMapper(const LinquCoordinate& c) : coord_(c) {}
    LinquCoordinate map(const std::string&) const override {
        return coord_;
    }
private:
    LinquCoordinate coord_;
};

}

#endif
