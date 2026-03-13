#include "core/coordinate.h"
#include "core/level.h"
#include "transport/unix_socket_transport.h"
#include "transport/msg_types.h"
#include "transport/linqu_header.h"
#include "discovery/peer_registry.h"
#include "discovery/filesystem_discovery.h"
#include "daemon/storage.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

static void usage(const char* prog) {
    fprintf(stderr, "Usage: %s --command <discover|status|shutdown> [--base DIR]\n", prog);
}

static bool send_message(const std::string& sock_path,
                         linqu::MsgType msg_type,
                         const linqu::LinquCoordinate& sender,
                         const linqu::LinquCoordinate& target,
                         uint8_t sender_level,
                         const uint8_t* payload, size_t payload_len) {
    auto hdr = linqu::LinquHeader::make(
        msg_type, sender_level, sender, target,
        static_cast<uint32_t>(payload_len));

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    uint8_t wire[linqu::LinquHeader::WIRE_SIZE];
    hdr.serialize(wire);

    bool ok = true;
    if (write(fd, wire, linqu::LinquHeader::WIRE_SIZE) !=
        (ssize_t)linqu::LinquHeader::WIRE_SIZE) {
        ok = false;
    }
    if (ok && payload_len > 0 && payload) {
        if (write(fd, payload, payload_len) != (ssize_t)payload_len) {
            ok = false;
        }
    }
    close(fd);
    return ok;
}

int main(int argc, char** argv) {
    std::string command;
    std::string base_path;
    const char* base_env = std::getenv("LINQU_BASE");
    base_path = base_env ? base_env : "/tmp/linqu/linqu_vcluster_1024";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--command") == 0 && i + 1 < argc) {
            command = argv[++i];
        } else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            base_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    if (command.empty()) {
        usage(argv[0]);
        return 1;
    }

    linqu::PeerRegistry registry;
    linqu::FilesystemDiscovery discovery(base_path);
    discovery.discover(registry);

    int total = registry.count();
    fprintf(stderr, "linqu_orchestrator: discovered %d peers at base=%s\n",
            total, base_path.c_str());

    linqu::LinquCoordinate self_coord;
    self_coord.l6_idx = 0;

    if (command == "discover" || command == "status") {
        auto peers_l3 = registry.peers_at_level(3);
        fprintf(stdout, "{\n");
        fprintf(stdout, "  \"command\": \"%s\",\n", command.c_str());
        fprintf(stdout, "  \"base_path\": \"%s\",\n", base_path.c_str());
        fprintf(stdout, "  \"total_peers\": %d,\n", total);
        fprintf(stdout, "  \"l3_hosts\": %zu,\n", peers_l3.size());

        auto peers_l4 = registry.peers_at_level(4);
        auto peers_l5 = registry.peers_at_level(5);
        auto peers_l6 = registry.peers_at_level(6);
        fprintf(stdout, "  \"l4_pods\": %zu,\n", peers_l4.size());
        fprintf(stdout, "  \"l5_supernodes\": %zu,\n", peers_l5.size());
        fprintf(stdout, "  \"l6_clusters\": %zu,\n", peers_l6.size());

        int alive = 0;
        for (auto& p : peers_l3) {
            if (p.alive) alive++;
        }
        fprintf(stdout, "  \"l3_alive\": %d,\n", alive);
        fprintf(stdout, "  \"status\": \"%s\"\n",
                alive == (int)peers_l3.size() ? "healthy" : "degraded");
        fprintf(stdout, "}\n");

    } else if (command == "shutdown") {
        auto all = registry.peers_at_level(3);
        int sent = 0;
        linqu::ShutdownPayload sp;
        sp.graceful = 1;
        auto buf = sp.serialize();

        for (auto& p : all) {
            if (send_message(p.socket_path, linqu::MsgType::SHUTDOWN,
                             self_coord, p.coord, 6, buf.data(), buf.size())) {
                sent++;
            }
        }
        fprintf(stderr, "linqu_orchestrator: sent SHUTDOWN to %d/%zu daemons\n",
                sent, all.size());

    } else {
        fprintf(stderr, "Unknown command: %s\n", command.c_str());
        return 1;
    }

    return 0;
}
