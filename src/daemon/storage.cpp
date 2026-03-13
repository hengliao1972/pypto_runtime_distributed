#include "daemon/storage.h"
#include <sys/stat.h>
#include <string>

namespace linqu {

static void mkdir_recursive(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if (path[i] == '/' || i == path.size() - 1) {
            mkdir(cur.c_str(), 0755);
        }
    }
}

std::string node_dir_path(const std::string& base_path,
                          const LinquCoordinate& coord) {
    return base_path + "/L6_" + std::to_string(coord.l6_idx)
         + "/L5_" + std::to_string(coord.l5_idx)
         + "/L4_" + std::to_string(coord.l4_idx)
         + "/L3_" + std::to_string(coord.l3_idx);
}

NodeStoragePaths create_node_storage(const std::string& base_path,
                                     const LinquCoordinate& coord) {
    NodeStoragePaths paths;
    paths.node_dir = node_dir_path(base_path, coord);
    paths.code_cache_dir = paths.node_dir + "/code_cache";
    paths.data_cache_dir = paths.node_dir + "/data_cache";
    paths.logs_dir = paths.node_dir + "/logs";

    mkdir_recursive(paths.node_dir);
    mkdir_recursive(paths.code_cache_dir);
    mkdir_recursive(paths.data_cache_dir);
    mkdir_recursive(paths.logs_dir);

    return paths;
}

bool node_storage_exists(const std::string& base_path,
                         const LinquCoordinate& coord) {
    struct stat st;
    return stat(node_dir_path(base_path, coord).c_str(), &st) == 0
        && S_ISDIR(st.st_mode);
}

}
