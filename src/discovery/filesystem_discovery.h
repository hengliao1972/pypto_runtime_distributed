#ifndef LINQU_DISCOVERY_FILESYSTEM_DISCOVERY_H
#define LINQU_DISCOVERY_FILESYSTEM_DISCOVERY_H

#include "discovery/peer_registry.h"
#include <string>

namespace linqu {

class FilesystemDiscovery {
public:
    explicit FilesystemDiscovery(const std::string& base_path);

    int discover(PeerRegistry& registry);

private:
    void scan_dir(const std::string& dir, PeerRegistry& registry, int& count);
    bool parse_socket_path(const std::string& path,
                           LinquCoordinate& coord, uint8_t& level);

    std::string base_path_;
};

}

#endif
