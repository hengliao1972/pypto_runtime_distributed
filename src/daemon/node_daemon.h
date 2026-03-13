#ifndef LINQU_DAEMON_NODE_DAEMON_H
#define LINQU_DAEMON_NODE_DAEMON_H

#include "core/coordinate.h"
#include "core/level.h"
#include "transport/unix_socket_transport.h"
#include "transport/msg_types.h"
#include "discovery/peer_registry.h"
#include "discovery/filesystem_discovery.h"
#include "daemon/code_cache.h"
#include "daemon/data_cache.h"
#include "runtime/linqu_orchestrator_state.h"
#include "profiling/ring_metrics.h"
#include <atomic>
#include <string>
#include <vector>

namespace linqu {

class NodeDaemon {
public:
    NodeDaemon(Level level,
               const LinquCoordinate& coord,
               const std::string& base_path);
    ~NodeDaemon();

    bool start();
    void stop();

    void run_event_loop(int max_messages = -1);

    bool handle_one_message(int timeout_ms = 5000);

    Level level() const { return level_; }
    const LinquCoordinate& coord() const { return coord_; }
    const std::string& storage_path() const { return storage_path_; }
    PeerRegistry& peer_registry() { return registry_; }
    CodeCache& code_cache() { return code_cache_; }
    DataCache& data_cache() { return data_cache_; }

    int messages_handled() const { return messages_handled_; }
    bool is_running() const { return running_.load(); }

private:
    void handle_call_task(const LinquHeader& hdr, const std::vector<uint8_t>& payload);
    void handle_shutdown(const LinquHeader& hdr, const std::vector<uint8_t>& payload);
    void handle_reg_code(const LinquHeader& hdr, const std::vector<uint8_t>& payload);
    void handle_reg_data(const LinquHeader& hdr, const std::vector<uint8_t>& payload);
    void handle_scope_exit(const LinquHeader& hdr, const std::vector<uint8_t>& payload);
    void handle_heartbeat(const LinquHeader& hdr, const std::vector<uint8_t>& payload);

    void send_task_complete(const LinquCoordinate& sender_coord,
                            uint8_t sender_level,
                            uint32_t task_id, int32_t status);

    Level level_;
    LinquCoordinate coord_;
    std::string base_path_;
    std::string storage_path_;

    UnixSocketTransport transport_;
    PeerRegistry registry_;
    CodeCache code_cache_;
    DataCache data_cache_;

    std::atomic<bool> running_{false};
    int messages_handled_ = 0;

    std::vector<ProfileReport> task_profiles_;
    void write_profile_json();
};

}

#endif
