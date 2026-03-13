#ifndef LINQU_DAEMON_STORAGE_H
#define LINQU_DAEMON_STORAGE_H

#include "core/coordinate.h"
#include <string>

namespace linqu {

struct NodeStoragePaths {
    std::string node_dir;
    std::string code_cache_dir;
    std::string data_cache_dir;
    std::string logs_dir;
};

NodeStoragePaths create_node_storage(const std::string& base_path,
                                     const LinquCoordinate& coord);

std::string node_dir_path(const std::string& base_path,
                          const LinquCoordinate& coord);

bool node_storage_exists(const std::string& base_path,
                         const LinquCoordinate& coord);

}

#endif
