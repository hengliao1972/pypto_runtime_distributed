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

struct RegCodePayload {
    uint64_t blob_hash = 0;
    std::vector<uint8_t> code_binary;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf(8 + 4 + code_binary.size());
        size_t off = 0;
        uint64_t bh = __builtin_bswap64(blob_hash);
        memcpy(buf.data() + off, &bh, 8); off += 8;
        uint32_t sz = htonl(static_cast<uint32_t>(code_binary.size()));
        memcpy(buf.data() + off, &sz, 4); off += 4;
        if (!code_binary.empty())
            memcpy(buf.data() + off, code_binary.data(), code_binary.size());
        return buf;
    }

    static RegCodePayload deserialize(const uint8_t* data, size_t len) {
        RegCodePayload p;
        if (len < 12) return p;
        size_t off = 0;
        uint64_t bh;
        memcpy(&bh, data + off, 8); p.blob_hash = __builtin_bswap64(bh); off += 8;
        uint32_t sz;
        memcpy(&sz, data + off, 4); sz = ntohl(sz); off += 4;
        if (off + sz <= len) {
            p.code_binary.assign(data + off, data + off + sz);
        }
        return p;
    }
};

struct RegDataPayload {
    uint64_t data_handle = 0;
    uint16_t scope_depth = 0;
    std::vector<uint8_t> buffer_bytes;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf(8 + 2 + 4 + buffer_bytes.size());
        size_t off = 0;
        uint64_t dh = __builtin_bswap64(data_handle);
        memcpy(buf.data() + off, &dh, 8); off += 8;
        uint16_t sd = htons(scope_depth);
        memcpy(buf.data() + off, &sd, 2); off += 2;
        uint32_t sz = htonl(static_cast<uint32_t>(buffer_bytes.size()));
        memcpy(buf.data() + off, &sz, 4); off += 4;
        if (!buffer_bytes.empty())
            memcpy(buf.data() + off, buffer_bytes.data(), buffer_bytes.size());
        return buf;
    }

    static RegDataPayload deserialize(const uint8_t* data, size_t len) {
        RegDataPayload p;
        if (len < 14) return p;
        size_t off = 0;
        uint64_t dh;
        memcpy(&dh, data + off, 8); p.data_handle = __builtin_bswap64(dh); off += 8;
        uint16_t sd;
        memcpy(&sd, data + off, 2); p.scope_depth = ntohs(sd); off += 2;
        uint32_t sz;
        memcpy(&sz, data + off, 4); sz = ntohl(sz); off += 4;
        if (off + sz <= len) {
            p.buffer_bytes.assign(data + off, data + off + sz);
        }
        return p;
    }
};

struct ScopeExitPayload {
    uint16_t scope_depth = 0;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf(2);
        uint16_t sd = htons(scope_depth);
        memcpy(buf.data(), &sd, 2);
        return buf;
    }

    static ScopeExitPayload deserialize(const uint8_t* data, size_t len) {
        ScopeExitPayload p;
        if (len >= 2) {
            uint16_t sd;
            memcpy(&sd, data, 2);
            p.scope_depth = ntohs(sd);
        }
        return p;
    }
};

struct RetryWithCodePayload {
    uint64_t blob_hash = 0;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf(8);
        uint64_t bh = __builtin_bswap64(blob_hash);
        memcpy(buf.data(), &bh, 8);
        return buf;
    }

    static RetryWithCodePayload deserialize(const uint8_t* data, size_t len) {
        RetryWithCodePayload p;
        if (len >= 8) {
            uint64_t bh;
            memcpy(&bh, data, 8);
            p.blob_hash = __builtin_bswap64(bh);
        }
        return p;
    }
};

struct HeartbeatPayload {
    uint8_t sender_l6 = 0;
    uint8_t sender_l5 = 0;
    uint8_t sender_l4 = 0;
    uint16_t sender_l3 = 0;
    uint8_t status = 0;  // 0=live, 1=suspect, 2=dead
    uint64_t timestamp = 0;
    std::string logical_system;
    std::string physical_system;

    std::vector<uint8_t> serialize() const {
        uint32_t ls_len = static_cast<uint32_t>(logical_system.size());
        uint32_t ps_len = static_cast<uint32_t>(physical_system.size());
        std::vector<uint8_t> buf(4 + 1 + 8 + 4 + ls_len + 4 + ps_len);
        size_t off = 0;
        buf[off++] = sender_l6;
        buf[off++] = sender_l5;
        buf[off++] = sender_l4;
        uint16_t l3 = htons(sender_l3);
        memcpy(buf.data() + off, &l3, 2); off += 2;
        buf[off - 1] = buf[off - 1]; // already correct from above
        // re-layout: l6,l5,l4 = 3 bytes, l3 = 2 bytes, status = 1 byte, ts = 8 bytes
        // let's simplify
        buf.clear();
        buf.resize(3 + 2 + 1 + 8 + 4 + ls_len + 4 + ps_len);
        off = 0;
        buf[off++] = sender_l6;
        buf[off++] = sender_l5;
        buf[off++] = sender_l4;
        l3 = htons(sender_l3);
        memcpy(buf.data() + off, &l3, 2); off += 2;
        buf[off++] = status;
        uint64_t ts = __builtin_bswap64(timestamp);
        memcpy(buf.data() + off, &ts, 8); off += 8;
        uint32_t nl = htonl(ls_len);
        memcpy(buf.data() + off, &nl, 4); off += 4;
        memcpy(buf.data() + off, logical_system.data(), ls_len); off += ls_len;
        nl = htonl(ps_len);
        memcpy(buf.data() + off, &nl, 4); off += 4;
        memcpy(buf.data() + off, physical_system.data(), ps_len); off += ps_len;
        buf.resize(off);
        return buf;
    }

    static HeartbeatPayload deserialize(const uint8_t* data, size_t len) {
        HeartbeatPayload p;
        if (len < 14) return p;
        size_t off = 0;
        p.sender_l6 = data[off++];
        p.sender_l5 = data[off++];
        p.sender_l4 = data[off++];
        uint16_t l3;
        memcpy(&l3, data + off, 2); p.sender_l3 = ntohs(l3); off += 2;
        p.status = data[off++];
        uint64_t ts;
        memcpy(&ts, data + off, 8); p.timestamp = __builtin_bswap64(ts); off += 8;
        if (off + 4 <= len) {
            uint32_t nl;
            memcpy(&nl, data + off, 4); nl = ntohl(nl); off += 4;
            if (off + nl <= len) {
                p.logical_system.assign(reinterpret_cast<const char*>(data + off), nl);
                off += nl;
            }
        }
        if (off + 4 <= len) {
            uint32_t nl;
            memcpy(&nl, data + off, 4); nl = ntohl(nl); off += 4;
            if (off + nl <= len) {
                p.physical_system.assign(reinterpret_cast<const char*>(data + off), nl);
                off += nl;
            }
        }
        return p;
    }
};

struct TensorLookupPayload {
    uint64_t handle = 0;
    uint8_t requestor_l6 = 0;
    uint8_t requestor_l5 = 0;
    uint8_t requestor_l4 = 0;
    uint16_t requestor_l3 = 0;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf(8 + 5);
        size_t off = 0;
        uint64_t h = __builtin_bswap64(handle);
        memcpy(buf.data() + off, &h, 8); off += 8;
        buf[off++] = requestor_l6;
        buf[off++] = requestor_l5;
        buf[off++] = requestor_l4;
        uint16_t l3 = htons(requestor_l3);
        memcpy(buf.data() + off, &l3, 2);
        return buf;
    }

    static TensorLookupPayload deserialize(const uint8_t* data, size_t len) {
        TensorLookupPayload p;
        if (len < 13) return p;
        size_t off = 0;
        uint64_t h;
        memcpy(&h, data + off, 8); p.handle = __builtin_bswap64(h); off += 8;
        p.requestor_l6 = data[off++];
        p.requestor_l5 = data[off++];
        p.requestor_l4 = data[off++];
        uint16_t l3;
        memcpy(&l3, data + off, 2); p.requestor_l3 = ntohs(l3);
        return p;
    }
};

struct TensorDataPayload {
    uint64_t handle = 0;
    int32_t producer_task_id = -1;
    std::vector<uint8_t> data;

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buf(8 + 4 + 4 + data.size());
        size_t off = 0;
        uint64_t h = __builtin_bswap64(handle);
        memcpy(buf.data() + off, &h, 8); off += 8;
        uint32_t pt = htonl(static_cast<uint32_t>(producer_task_id));
        memcpy(buf.data() + off, &pt, 4); off += 4;
        uint32_t sz = htonl(static_cast<uint32_t>(data.size()));
        memcpy(buf.data() + off, &sz, 4); off += 4;
        if (!data.empty())
            memcpy(buf.data() + off, data.data(), data.size());
        return buf;
    }

    static TensorDataPayload deserialize(const uint8_t* d, size_t len) {
        TensorDataPayload p;
        if (len < 16) return p;
        size_t off = 0;
        uint64_t h;
        memcpy(&h, d + off, 8); p.handle = __builtin_bswap64(h); off += 8;
        uint32_t pt;
        memcpy(&pt, d + off, 4); p.producer_task_id = static_cast<int32_t>(ntohl(pt)); off += 4;
        uint32_t sz;
        memcpy(&sz, d + off, 4); sz = ntohl(sz); off += 4;
        if (off + sz <= len) {
            p.data.assign(d + off, d + off + sz);
        }
        return p;
    }
};

}

#endif
