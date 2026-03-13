#ifndef LINQU_TRANSPORT_TRANSPORT_H
#define LINQU_TRANSPORT_TRANSPORT_H

#include "transport/linqu_header.h"
#include <cstdint>
#include <vector>

namespace linqu {

class Transport {
public:
    virtual ~Transport() = default;

    virtual bool send(const LinquCoordinate& target,
                      const LinquHeader& hdr,
                      const uint8_t* payload, size_t len) = 0;

    virtual bool recv(LinquHeader& hdr,
                      std::vector<uint8_t>& payload,
                      int timeout_ms = -1) = 0;

    virtual bool is_listening() const = 0;
};

}

#endif
