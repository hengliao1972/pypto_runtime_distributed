#ifndef LINQU_DISCOVERY_PEER_REGISTRY_H
#define LINQU_DISCOVERY_PEER_REGISTRY_H

#include "core/coordinate.h"
#include "core/level.h"
#include <cstdint>
#include <string>
#include <vector>
#include <map>
#include <mutex>

namespace linqu {

struct PeerInfo {
    LinquCoordinate coord;
    uint8_t         level;
    std::string     socket_path;
    bool            alive = true;
};

class PeerRegistry {
public:
    void add(const PeerInfo& peer);

    void remove(const LinquCoordinate& coord, uint8_t level);

    void set_alive(const LinquCoordinate& coord, uint8_t level, bool alive);

    std::vector<PeerInfo> peers_at_level(uint8_t level) const;

    std::vector<PeerInfo> peers_same_parent(const LinquCoordinate& self,
                                            uint8_t child_level) const;

    std::vector<PeerInfo> all_peers() const;

    size_t count() const;

    void clear();

private:
    struct CoordLevelKey {
        LinquCoordinate coord;
        uint8_t level;
        bool operator<(const CoordLevelKey& o) const {
            if (level != o.level) return level < o.level;
            return coord < o.coord;
        }
    };

    mutable std::mutex mu_;
    std::map<CoordLevelKey, PeerInfo> peers_;
};

}

#endif
