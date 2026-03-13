#include "transport/unix_socket_transport.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>
#include <cstdio>
#include <cstring>

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

std::string UnixSocketTransport::make_socket_path(const std::string& base_path,
                                                   const LinquCoordinate& coord,
                                                   uint8_t level) {
    std::string p = base_path;
    if (p.empty()) p = "/tmp/linqu";
    if (p.back() != '/') p += '/';
    p += "L6_" + std::to_string(coord.l6_idx) + "/";
    if (level <= 5) p += "L5_" + std::to_string(coord.l5_idx) + "/";
    if (level <= 4) p += "L4_" + std::to_string(coord.l4_idx) + "/";
    if (level <= 3) p += "L3_" + std::to_string(coord.l3_idx) + "/";
    p += "daemon_L" + std::to_string(level) + ".sock";
    return p;
}

UnixSocketTransport::UnixSocketTransport(const std::string& base_path,
                                         const LinquCoordinate& self_coord,
                                         uint8_t self_level)
    : base_path_(base_path), self_coord_(self_coord), self_level_(self_level) {
    socket_path_ = make_socket_path(base_path, self_coord, self_level);
}

UnixSocketTransport::~UnixSocketTransport() {
    stop();
}

bool UnixSocketTransport::start_listening() {
    if (listen_fd_ >= 0) return true;

    std::string dir = socket_path_.substr(0, socket_path_.rfind('/'));
    mkdir_p(dir);

    unlink(socket_path_.c_str());

    listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        perror("socket");
        return false;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() >= sizeof(addr.sun_path)) {
        fprintf(stderr, "Socket path too long: %s\n", socket_path_.c_str());
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (listen(listen_fd_, 32) < 0) {
        perror("listen");
        close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    stopped_ = false;
    return true;
}

void UnixSocketTransport::stop() {
    stopped_ = true;
    if (listen_fd_ >= 0) {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    unlink(socket_path_.c_str());
}

bool UnixSocketTransport::send_all(int fd, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::write(fd, p + sent, len - sent);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool UnixSocketTransport::recv_all(int fd, void* data, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(data);
    size_t got = 0;
    while (got < len) {
        ssize_t n = ::read(fd, p + got, len - got);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

bool UnixSocketTransport::send(const LinquCoordinate& target,
                                const LinquHeader& hdr,
                                const uint8_t* payload, size_t len) {
    uint8_t target_level = static_cast<uint8_t>(hdr.msg_type == MsgType::TASK_COMPLETE
                                                 ? hdr.sender_level
                                                 : hdr.sender_level - 1);

    std::string target_path = make_socket_path(base_path_, target, target_level);

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, target_path.c_str(), sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }

    uint8_t wire[LinquHeader::WIRE_SIZE];
    hdr.serialize(wire);

    bool ok = send_all(fd, wire, LinquHeader::WIRE_SIZE);
    if (ok && len > 0 && payload) {
        ok = send_all(fd, payload, len);
    }
    close(fd);
    return ok;
}

bool UnixSocketTransport::recv(LinquHeader& hdr,
                                std::vector<uint8_t>& payload,
                                int timeout_ms) {
    if (listen_fd_ < 0 || stopped_) return false;

    struct pollfd pfd;
    pfd.fd = listen_fd_;
    pfd.events = POLLIN;

    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0 || stopped_) return false;

    int client_fd = accept(listen_fd_, nullptr, nullptr);
    if (client_fd < 0) return false;

    uint8_t wire[LinquHeader::WIRE_SIZE];
    bool ok = recv_all(client_fd, wire, LinquHeader::WIRE_SIZE);
    if (!ok) { close(client_fd); return false; }

    hdr = LinquHeader::deserialize(wire);

    if (hdr.magic != LINQU_MAGIC) {
        close(client_fd);
        return false;
    }

    if (hdr.payload_size > 0) {
        payload.resize(hdr.payload_size);
        ok = recv_all(client_fd, payload.data(), hdr.payload_size);
    } else {
        payload.clear();
    }

    close(client_fd);
    return ok;
}

}
