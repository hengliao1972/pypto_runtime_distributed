#include "discovery/peer_registry.h"

namespace linqu {

void PeerRegistry::add(const PeerInfo& peer) {
    std::lock_guard<std::mutex> lk(mu_);
    CoordLevelKey key{peer.coord, peer.level};
    peers_[key] = peer;
}

void PeerRegistry::remove(const LinquCoordinate& coord, uint8_t level) {
    std::lock_guard<std::mutex> lk(mu_);
    peers_.erase(CoordLevelKey{coord, level});
}

void PeerRegistry::set_alive(const LinquCoordinate& coord, uint8_t level, bool alive) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = peers_.find(CoordLevelKey{coord, level});
    if (it != peers_.end()) {
        it->second.alive = alive;
    }
}

std::vector<PeerInfo> PeerRegistry::peers_at_level(uint8_t level) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<PeerInfo> result;
    for (auto& [key, info] : peers_) {
        if (key.level == level && info.alive) {
            result.push_back(info);
        }
    }
    return result;
}

std::vector<PeerInfo> PeerRegistry::peers_same_parent(const LinquCoordinate& self,
                                                       uint8_t child_level) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<PeerInfo> result;
    for (auto& [key, info] : peers_) {
        if (key.level != child_level || !info.alive) continue;

        bool same_parent = true;
        for (int lv = 6; lv > child_level; lv--) {
            auto l = static_cast<Level>(lv);
            if (info.coord.index_at(l) != self.index_at(l)) {
                same_parent = false;
                break;
            }
        }
        if (same_parent) {
            result.push_back(info);
        }
    }
    return result;
}

std::vector<PeerInfo> PeerRegistry::all_peers() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<PeerInfo> result;
    for (auto& [_, info] : peers_) {
        result.push_back(info);
    }
    return result;
}

size_t PeerRegistry::count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return peers_.size();
}

void PeerRegistry::clear() {
    std::lock_guard<std::mutex> lk(mu_);
    peers_.clear();
}

}
