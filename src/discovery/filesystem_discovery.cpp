#include "discovery/filesystem_discovery.h"
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <regex>

namespace linqu {

FilesystemDiscovery::FilesystemDiscovery(const std::string& base_path)
    : base_path_(base_path) {}

int FilesystemDiscovery::discover(PeerRegistry& registry) {
    int count = 0;
    scan_dir(base_path_, registry, count);
    return count;
}

void FilesystemDiscovery::scan_dir(const std::string& dir,
                                    PeerRegistry& registry, int& count) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string name(ent->d_name);
        std::string full = dir + "/" + name;

        struct stat st;
        if (lstat(full.c_str(), &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_dir(full, registry, count);
        } else if (S_ISSOCK(st.st_mode) || name.find("daemon_L") == 0) {
            LinquCoordinate coord;
            uint8_t level;
            if (parse_socket_path(full, coord, level)) {
                PeerInfo info;
                info.coord = coord;
                info.level = level;
                info.socket_path = full;
                info.alive = true;
                registry.add(info);
                count++;
            }
        }
    }
    closedir(d);
}

bool FilesystemDiscovery::parse_socket_path(const std::string& path,
                                             LinquCoordinate& coord,
                                             uint8_t& level) {
    // Extract level from filename: daemon_L<n>.sock
    auto fname_pos = path.rfind('/');
    if (fname_pos == std::string::npos) return false;
    std::string fname = path.substr(fname_pos + 1);

    static std::regex sock_re("daemon_L(\\d+)\\.sock");
    std::smatch m;
    if (!std::regex_match(fname, m, sock_re)) return false;
    level = static_cast<uint8_t>(std::atoi(m[1].str().c_str()));

    // Extract coordinates from directory path: .../L6_<n>/L5_<n>/L4_<n>/L3_<n>/
    coord = LinquCoordinate{};
    static std::regex coord_re("L(\\d+)_(\\d+)");
    std::string dir_part = path.substr(0, fname_pos);
    auto it = std::sregex_iterator(dir_part.begin(), dir_part.end(), coord_re);
    auto end = std::sregex_iterator();
    for (; it != end; ++it) {
        int lv = std::atoi((*it)[1].str().c_str());
        int idx = std::atoi((*it)[2].str().c_str());
        switch (lv) {
            case 6: coord.l6_idx = static_cast<uint8_t>(idx); break;
            case 5: coord.l5_idx = static_cast<uint8_t>(idx); break;
            case 4: coord.l4_idx = static_cast<uint8_t>(idx); break;
            case 3: coord.l3_idx = static_cast<uint16_t>(idx); break;
            case 2: coord.l2_idx = static_cast<uint16_t>(idx); break;
            case 1: coord.l1_idx = static_cast<uint8_t>(idx); break;
            case 0: coord.l0_idx = static_cast<uint16_t>(idx); break;
        }
    }
    return true;
}

}
