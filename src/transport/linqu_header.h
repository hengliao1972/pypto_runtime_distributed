#ifndef LINQU_TRANSPORT_HEADER_H
#define LINQU_TRANSPORT_HEADER_H

#include "core/coordinate.h"
#include <cstdint>
#include <cstring>
#include <arpa/inet.h>

namespace linqu {

enum class MsgType : uint8_t {
    HEARTBEAT       = 0,
    REG_CODE        = 1,
    REG_DATA        = 2,
    CALL_TASK       = 3,
    SCOPE_EXIT      = 4,
    RETRY_WITH_CODE = 5,
    TASK_COMPLETE   = 6,
    SHUTDOWN        = 7,
};

static constexpr uint32_t LINQU_MAGIC = 0x4C51524Du;  // "LQRM"

struct LinquHeader {
    uint32_t magic;
    uint8_t  version;
    MsgType  msg_type;
    uint8_t  sender_level;
    uint8_t  reserved;
    uint32_t payload_size;
    uint8_t  sender_l6;
    uint8_t  sender_l5;
    uint8_t  sender_l4;
    uint8_t  sender_l3_hi;
    uint8_t  sender_l3_lo;
    uint8_t  target_l6;
    uint8_t  target_l5;
    uint8_t  target_l4;
    uint8_t  target_l3_hi;
    uint8_t  target_l3_lo;
    uint8_t  padding[2];

    static constexpr size_t WIRE_SIZE = 24;

    void set_sender(const LinquCoordinate& c) {
        sender_l6 = c.l6_idx;
        sender_l5 = c.l5_idx;
        sender_l4 = c.l4_idx;
        sender_l3_hi = static_cast<uint8_t>(c.l3_idx >> 8);
        sender_l3_lo = static_cast<uint8_t>(c.l3_idx & 0xFF);
    }

    LinquCoordinate get_sender() const {
        LinquCoordinate c;
        c.l6_idx = sender_l6;
        c.l5_idx = sender_l5;
        c.l4_idx = sender_l4;
        c.l3_idx = static_cast<uint16_t>((sender_l3_hi << 8) | sender_l3_lo);
        return c;
    }

    void set_target(const LinquCoordinate& c) {
        target_l6 = c.l6_idx;
        target_l5 = c.l5_idx;
        target_l4 = c.l4_idx;
        target_l3_hi = static_cast<uint8_t>(c.l3_idx >> 8);
        target_l3_lo = static_cast<uint8_t>(c.l3_idx & 0xFF);
    }

    LinquCoordinate get_target() const {
        LinquCoordinate c;
        c.l6_idx = target_l6;
        c.l5_idx = target_l5;
        c.l4_idx = target_l4;
        c.l3_idx = static_cast<uint16_t>((target_l3_hi << 8) | target_l3_lo);
        return c;
    }

    void serialize(uint8_t buf[WIRE_SIZE]) const {
        uint32_t net_magic = htonl(magic);
        uint32_t net_payload = htonl(payload_size);
        memcpy(buf + 0, &net_magic, 4);
        buf[4] = version;
        buf[5] = static_cast<uint8_t>(msg_type);
        buf[6] = sender_level;
        buf[7] = reserved;
        memcpy(buf + 8, &net_payload, 4);
        buf[12] = sender_l6;
        buf[13] = sender_l5;
        buf[14] = sender_l4;
        buf[15] = sender_l3_hi;
        buf[16] = sender_l3_lo;
        buf[17] = target_l6;
        buf[18] = target_l5;
        buf[19] = target_l4;
        buf[20] = target_l3_hi;
        buf[21] = target_l3_lo;
        buf[22] = padding[0];
        buf[23] = padding[1];
    }

    static LinquHeader deserialize(const uint8_t buf[WIRE_SIZE]) {
        LinquHeader h;
        uint32_t net_magic, net_payload;
        memcpy(&net_magic, buf + 0, 4);
        h.magic = ntohl(net_magic);
        h.version = buf[4];
        h.msg_type = static_cast<MsgType>(buf[5]);
        h.sender_level = buf[6];
        h.reserved = buf[7];
        memcpy(&net_payload, buf + 8, 4);
        h.payload_size = ntohl(net_payload);
        h.sender_l6 = buf[12];
        h.sender_l5 = buf[13];
        h.sender_l4 = buf[14];
        h.sender_l3_hi = buf[15];
        h.sender_l3_lo = buf[16];
        h.target_l6 = buf[17];
        h.target_l5 = buf[18];
        h.target_l4 = buf[19];
        h.target_l3_hi = buf[20];
        h.target_l3_lo = buf[21];
        h.padding[0] = buf[22];
        h.padding[1] = buf[23];
        return h;
    }

    static LinquHeader make(MsgType type, uint8_t sender_level,
                            const LinquCoordinate& sender,
                            const LinquCoordinate& target,
                            uint32_t payload_size) {
        LinquHeader h;
        memset(&h, 0, sizeof(h));
        h.magic = LINQU_MAGIC;
        h.version = 1;
        h.msg_type = type;
        h.sender_level = sender_level;
        h.payload_size = payload_size;
        h.set_sender(sender);
        h.set_target(target);
        return h;
    }
};

static_assert(LinquHeader::WIRE_SIZE == 24, "LinquHeader wire size must be 24 bytes");

}

#endif
