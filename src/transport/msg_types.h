#ifndef LINQU_TRANSPORT_MSG_TYPES_H
#define LINQU_TRANSPORT_MSG_TYPES_H

#include "transport/linqu_header.h"
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace linqu {

struct CallTaskPayload {
    std::string kernel_so_name;
    uint32_t task_id = 0;
    uint16_t scope_depth = 0;
    uint32_t num_params = 0;
    struct ParamEntry {
        uint8_t  type;
        uint64_t handle;
        uint64_t scalar_value;
    };
    std::vector<ParamEntry> params;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf;
        uint32_t name_len = static_cast<uint32_t>(kernel_so_name.size());
        buf.resize(4 + name_len + 4 + 2 + 4);
        size_t off = 0;

        uint32_t nl = htonl(name_len);
        memcpy(buf.data() + off, &nl, 4); off += 4;
        memcpy(buf.data() + off, kernel_so_name.data(), name_len); off += name_len;

        uint32_t tid = htonl(task_id);
        memcpy(buf.data() + off, &tid, 4); off += 4;

        uint16_t sd = htons(scope_depth);
        memcpy(buf.data() + off, &sd, 2); off += 2;

        uint32_t np = htonl(num_params);
        memcpy(buf.data() + off, &np, 4); off += 4;

        for (uint32_t i = 0; i < num_params && i < params.size(); i++) {
            size_t old_sz = buf.size();
            buf.resize(old_sz + 1 + 8 + 8);
            buf[old_sz] = params[i].type;
            uint64_t h_be;
            h_be = __builtin_bswap64(params[i].handle);
            memcpy(buf.data() + old_sz + 1, &h_be, 8);
            h_be = __builtin_bswap64(params[i].scalar_value);
            memcpy(buf.data() + old_sz + 9, &h_be, 8);
        }
        return buf;
    }

    static CallTaskPayload deserialize(const uint8_t* data, size_t len) {
        CallTaskPayload p;
        size_t off = 0;
        if (len < 14) return p;

        uint32_t nl;
        memcpy(&nl, data + off, 4); nl = ntohl(nl); off += 4;
        if (off + nl > len) return p;
        p.kernel_so_name.assign(reinterpret_cast<const char*>(data + off), nl);
        off += nl;

        uint32_t tid;
        memcpy(&tid, data + off, 4); p.task_id = ntohl(tid); off += 4;

        uint16_t sd;
        memcpy(&sd, data + off, 2); p.scope_depth = ntohs(sd); off += 2;

        uint32_t np;
        memcpy(&np, data + off, 4); p.num_params = ntohl(np); off += 4;

        for (uint32_t i = 0; i < p.num_params && off + 17 <= len; i++) {
            ParamEntry e;
            e.type = data[off]; off += 1;
            uint64_t h_be;
            memcpy(&h_be, data + off, 8);
            e.handle = __builtin_bswap64(h_be); off += 8;
            memcpy(&h_be, data + off, 8);
            e.scalar_value = __builtin_bswap64(h_be); off += 8;
            p.params.push_back(e);
        }
        return p;
    }
};

struct TaskCompletePayload {
    uint32_t task_id = 0;
    int32_t  status = 0;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf(8);
        uint32_t tid = htonl(task_id);
        memcpy(buf.data(), &tid, 4);
        uint32_t st = htonl(static_cast<uint32_t>(status));
        memcpy(buf.data() + 4, &st, 4);
        return buf;
    }

    static TaskCompletePayload deserialize(const uint8_t* data, size_t len) {
        TaskCompletePayload p;
        if (len < 8) return p;
        uint32_t tid, st;
        memcpy(&tid, data, 4); p.task_id = ntohl(tid);
        memcpy(&st, data + 4, 4); p.status = static_cast<int32_t>(ntohl(st));
        return p;
    }
};

struct ShutdownPayload {
    uint8_t graceful = 1;

    std::vector<uint8_t> serialize() const {
        return {graceful};
    }

    static ShutdownPayload deserialize(const uint8_t* data, size_t len) {
        ShutdownPayload p;
        if (len >= 1) p.graceful = data[0];
        return p;
    }
};

}

#endif
