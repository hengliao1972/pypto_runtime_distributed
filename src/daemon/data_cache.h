#ifndef LINQU_DAEMON_DATA_CACHE_H
#define LINQU_DAEMON_DATA_CACHE_H

#include <cstdint>
#include <cstddef>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <utility>

namespace linqu {

class DataCache {
public:
    void register_data(uint64_t handle, const void* data, size_t size) {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<uint8_t> buf(static_cast<const uint8_t*>(data),
                                  static_cast<const uint8_t*>(data) + size);
        entries_[handle] = std::move(buf);
    }

    std::pair<const void*, size_t> lookup(uint64_t handle) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = entries_.find(handle);
        if (it == entries_.end()) return {nullptr, 0};
        return {it->second.data(), it->second.size()};
    }

    bool has(uint64_t handle) const {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_.count(handle) > 0;
    }

    void remove(uint64_t handle) {
        std::lock_guard<std::mutex> lock(mu_);
        entries_.erase(handle);
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mu_);
        entries_.clear();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mu_);
        return entries_.size();
    }

private:
    mutable std::mutex mu_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> entries_;
};

}

#endif
