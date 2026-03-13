#include "daemon/storage.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

static bool dir_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

int main() {
    using namespace linqu;
    std::string base = "/tmp/linqu_test_storage_" + std::to_string(getpid());

    printf("=== test_storage ===\n");

    // Test 1: create_node_storage
    {
        LinquCoordinate coord;
        coord.l6_idx = 0; coord.l5_idx = 7; coord.l4_idx = 2; coord.l3_idx = 11;

        auto paths = create_node_storage(base, coord);
        assert(dir_exists(paths.node_dir));
        assert(dir_exists(paths.code_cache_dir));
        assert(dir_exists(paths.data_cache_dir));
        assert(dir_exists(paths.logs_dir));

        assert(paths.node_dir.find("L6_0/L5_7/L4_2/L3_11") != std::string::npos);
        printf("  [PASS] Test 1: create_node_storage creates correct dirs\n");
    }

    // Test 2: node_storage_exists
    {
        LinquCoordinate coord;
        coord.l6_idx = 0; coord.l5_idx = 7; coord.l4_idx = 2; coord.l3_idx = 11;
        assert(node_storage_exists(base, coord));

        LinquCoordinate coord2;
        coord2.l6_idx = 0; coord2.l5_idx = 15; coord2.l4_idx = 3; coord2.l3_idx = 0;
        assert(!node_storage_exists(base, coord2));
        printf("  [PASS] Test 2: node_storage_exists\n");
    }

    // Test 3: create multiple nodes
    {
        int count = 0;
        for (int l5 = 0; l5 < 2; l5++) {
            for (int l4 = 0; l4 < 2; l4++) {
                for (int l3 = 0; l3 < 4; l3++) {
                    LinquCoordinate c;
                    c.l5_idx = static_cast<uint8_t>(l5);
                    c.l4_idx = static_cast<uint8_t>(l4);
                    c.l3_idx = static_cast<uint16_t>(l3);
                    create_node_storage(base, c);
                    count++;
                }
            }
        }
        assert(count == 16);
        printf("  [PASS] Test 3: created %d node storage directories\n", count);
    }

    // Cleanup
    std::string rm_cmd = "rm -rf " + base;
    system(rm_cmd.c_str());

    printf("=== all storage tests passed ===\n");
    return 0;
}
