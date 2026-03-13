#include "daemon/node_daemon.h"
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace linqu {

static void mkdir_p(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if (path[i] == '/' || i == path.size() - 1) {
            mkdir(cur.c_str(), 0755);
        }
    }
}

NodeDaemon::NodeDaemon(Level level,
                       const LinquCoordinate& coord,
                       const std::string& base_path)
    : level_(level), coord_(coord), base_path_(base_path),
      transport_(base_path, coord, level_value(level)),
      code_cache_("") {
    storage_path_ = base_path + "/" +
        UnixSocketTransport::make_socket_path(base_path, coord, level_value(level));
    // Extract directory from socket path
    auto sock_path = UnixSocketTransport::make_socket_path(base_path, coord, level_value(level));
    auto dir_end = sock_path.rfind('/');
    if (dir_end != std::string::npos) {
        storage_path_ = sock_path.substr(0, dir_end);
    }
    code_cache_ = CodeCache(storage_path_);
}

NodeDaemon::~NodeDaemon() {
    stop();
}

bool NodeDaemon::start() {
    mkdir_p(storage_path_);
    if (!transport_.start_listening()) {
        fprintf(stderr, "[Daemon L%d %s] Failed to start listening\n",
                level_value(level_), coord_.to_string().c_str());
        return false;
    }
    running_ = true;
    fprintf(stderr, "[Daemon L%d %s pid=%d] Started at %s\n",
            level_value(level_), coord_.to_string().c_str(),
            getpid(), transport_.socket_path().c_str());
    return true;
}

void NodeDaemon::stop() {
    running_ = false;
    write_profile_json();
    transport_.stop();
    code_cache_.unload_all();
}

void NodeDaemon::run_event_loop(int max_messages) {
    int count = 0;
    while (running_) {
        if (max_messages >= 0 && count >= max_messages) break;
        if (handle_one_message(1000)) {
            count++;
        }
    }
}

bool NodeDaemon::handle_one_message(int timeout_ms) {
    LinquHeader hdr;
    std::vector<uint8_t> payload;
    if (!transport_.recv(hdr, payload, timeout_ms)) {
        return false;
    }

    if (hdr.magic != LINQU_MAGIC) {
        fprintf(stderr, "[Daemon L%d] Bad magic: 0x%08X\n",
                level_value(level_), hdr.magic);
        return false;
    }

    messages_handled_++;

    switch (hdr.msg_type) {
        case MsgType::CALL_TASK:
            handle_call_task(hdr, payload);
            return true;
        case MsgType::SHUTDOWN:
            handle_shutdown(hdr, payload);
            return true;
        case MsgType::HEARTBEAT:
            handle_heartbeat(hdr, payload);
            return true;
        case MsgType::REG_CODE:
            handle_reg_code(hdr, payload);
            return true;
        case MsgType::REG_DATA:
            handle_reg_data(hdr, payload);
            return true;
        case MsgType::SCOPE_EXIT:
            handle_scope_exit(hdr, payload);
            return true;
        default:
            fprintf(stderr, "[Daemon L%d] Unhandled msg_type=%d\n",
                    level_value(level_), static_cast<int>(hdr.msg_type));
            return true;
    }
}

void NodeDaemon::handle_call_task(const LinquHeader& hdr,
                                   const std::vector<uint8_t>& payload) {
    auto task = CallTaskPayload::deserialize(payload.data(), payload.size());
    fprintf(stderr, "[Daemon L%d %s] CALL_TASK: kernel=%s task_id=%u num_params=%u\n",
            level_value(level_), coord_.to_string().c_str(),
            task.kernel_so_name.c_str(), task.task_id, task.num_params);

    std::vector<uint64_t> args;
    std::vector<LinquParam> linqu_params;
    args.reserve(task.params.size());
    linqu_params.reserve(task.params.size());
    for (const auto& pe : task.params) {
        LinquParam lp;
        lp.type = static_cast<LinquParamType>(pe.type);
        lp.handle = pe.handle;
        lp.scalar_value = pe.scalar_value;
        linqu_params.push_back(lp);

        if (lp.type == LINQU_PARAM_SCALAR) {
            args.push_back(lp.scalar_value);
        } else {
            args.push_back(lp.handle);
        }
    }

    auto kernel = code_cache_.load(task.kernel_so_name);
    int32_t status = 0;

    if (kernel.entry_fn) {
        LinquOrchestratorState state;
        LinquOrchConfig_Internal cfg;
        state.init(level_, coord_, cfg);
        state.set_peer_registry(&registry_);

        LinquCoordinate_C self_c;
        self_c.l6_idx = coord_.l6_idx;
        self_c.l5_idx = coord_.l5_idx;
        self_c.l4_idx = coord_.l4_idx;
        self_c.l3_idx = coord_.l3_idx;
        self_c.l2_idx = coord_.l2_idx;
        self_c.l1_idx = coord_.l1_idx;
        self_c.l0_idx = coord_.l0_idx;

        for (const auto& lp : linqu_params) {
            if (lp.type == LINQU_PARAM_INPUT || lp.type == LINQU_PARAM_INOUT ||
                lp.type == LINQU_PARAM_OUTPUT) {
                state.alloc_tensor(self_c, 0);
            }
        }

        kernel.entry_fn(state.runtime(), args.data(),
                        static_cast<int>(args.size()));

        auto profile = state.generate_profile(coord_.to_string());
        task_profiles_.push_back(profile);
    } else {
        fprintf(stderr, "[Daemon L%d %s] Kernel '%s' not found, "
                "treating as mock execution\n",
                level_value(level_), coord_.to_string().c_str(),
                task.kernel_so_name.c_str());
    }

    send_task_complete(hdr.get_sender(), hdr.sender_level,
                       task.task_id, status);
}

void NodeDaemon::handle_shutdown(const LinquHeader&,
                                  const std::vector<uint8_t>&) {
    fprintf(stderr, "[Daemon L%d %s] SHUTDOWN received\n",
            level_value(level_), coord_.to_string().c_str());
    running_ = false;
}

void NodeDaemon::send_task_complete(const LinquCoordinate& sender_coord,
                                     uint8_t sender_level,
                                     uint32_t task_id, int32_t status) {
    TaskCompletePayload comp;
    comp.task_id = task_id;
    comp.status = status;
    auto buf = comp.serialize();

    LinquHeader resp = LinquHeader::make(
        MsgType::TASK_COMPLETE,
        level_value(level_), coord_, sender_coord,
        static_cast<uint32_t>(buf.size()));
    resp.sender_level = level_value(level_);

    std::string sender_sock = UnixSocketTransport::make_socket_path(
        base_path_, sender_coord, sender_level);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sender_sock.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return;
    }
    uint8_t wire[LinquHeader::WIRE_SIZE];
    resp.serialize(wire);
    (void)write(fd, wire, LinquHeader::WIRE_SIZE);
    (void)write(fd, buf.data(), buf.size());
    close(fd);
}

void NodeDaemon::write_profile_json() {
    if (task_profiles_.empty()) return;

    std::string logs_dir = storage_path_ + "/logs";
    mkdir_p(logs_dir);
    std::string path = logs_dir + "/ring_profile.json";

    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;

    fprintf(f, "[\n");
    for (size_t i = 0; i < task_profiles_.size(); i++) {
        std::string json = task_profiles_[i].to_json();
        fprintf(f, "%s%s", json.c_str(),
                i + 1 < task_profiles_.size() ? ",\n" : "\n");
    }
    fprintf(f, "]\n");
    fclose(f);

    fprintf(stderr, "[Daemon L%d %s] Wrote profile to %s (%zu tasks)\n",
            level_value(level_), coord_.to_string().c_str(),
            path.c_str(), task_profiles_.size());
}

void NodeDaemon::handle_reg_code(const LinquHeader&,
                                  const std::vector<uint8_t>& payload) {
    auto reg = RegCodePayload::deserialize(payload.data(), payload.size());
    fprintf(stderr, "[Daemon L%d %s] REG_CODE: hash=0x%lx size=%zu\n",
            level_value(level_), coord_.to_string().c_str(),
            static_cast<unsigned long>(reg.blob_hash), reg.code_binary.size());

    if (!reg.code_binary.empty()) {
        std::string name = "blob_" + std::to_string(reg.blob_hash) + ".so";
        code_cache_.register_code(name, reg.code_binary.data(), reg.code_binary.size());
    }
}

void NodeDaemon::handle_reg_data(const LinquHeader&,
                                  const std::vector<uint8_t>& payload) {
    auto reg = RegDataPayload::deserialize(payload.data(), payload.size());
    fprintf(stderr, "[Daemon L%d %s] REG_DATA: handle=0x%lx depth=%u size=%zu\n",
            level_value(level_), coord_.to_string().c_str(),
            static_cast<unsigned long>(reg.data_handle),
            reg.scope_depth, reg.buffer_bytes.size());

    if (!reg.buffer_bytes.empty()) {
        data_cache_.register_data(reg.data_handle,
                                   reg.buffer_bytes.data(),
                                   reg.buffer_bytes.size());
    }
}

void NodeDaemon::handle_scope_exit(const LinquHeader&,
                                    const std::vector<uint8_t>& payload) {
    auto se = ScopeExitPayload::deserialize(payload.data(), payload.size());
    fprintf(stderr, "[Daemon L%d %s] SCOPE_EXIT: depth=%u\n",
            level_value(level_), coord_.to_string().c_str(),
            se.scope_depth);
}

void NodeDaemon::handle_heartbeat(const LinquHeader&,
                                   const std::vector<uint8_t>& payload) {
    auto hb = HeartbeatPayload::deserialize(payload.data(), payload.size());
    PeerInfo info;
    info.coord.l6_idx = hb.sender_l6;
    info.coord.l5_idx = hb.sender_l5;
    info.coord.l4_idx = hb.sender_l4;
    info.coord.l3_idx = hb.sender_l3;
    info.level = level_value(level_);
    info.alive = (hb.status == 0);
    registry_.add(info);
}

}
