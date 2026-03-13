#ifndef LINQU_TRANSPORT_UNIX_SOCKET_TRANSPORT_H
#define LINQU_TRANSPORT_UNIX_SOCKET_TRANSPORT_H

#include "transport/transport.h"
#include <string>
#include <atomic>

namespace linqu {

class UnixSocketTransport : public Transport {
public:
    UnixSocketTransport(const std::string& base_path,
                        const LinquCoordinate& self_coord,
                        uint8_t self_level);
    ~UnixSocketTransport() override;

    bool start_listening();
    void stop();

    bool send(const LinquCoordinate& target,
              const LinquHeader& hdr,
              const uint8_t* payload, size_t len) override;

    bool recv(LinquHeader& hdr,
              std::vector<uint8_t>& payload,
              int timeout_ms = -1) override;

    bool is_listening() const override { return listen_fd_ >= 0; }

    std::string socket_path() const { return socket_path_; }

    static std::string make_socket_path(const std::string& base_path,
                                        const LinquCoordinate& coord,
                                        uint8_t level);

private:
    bool send_all(int fd, const void* data, size_t len);
    bool recv_all(int fd, void* data, size_t len);

    std::string base_path_;
    LinquCoordinate self_coord_;
    uint8_t self_level_;
    std::string socket_path_;
    int listen_fd_ = -1;
    std::atomic<bool> stopped_{false};
};

}

#endif
