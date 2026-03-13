#ifndef LINQU_DAEMON_CODE_CACHE_H
#define LINQU_DAEMON_CODE_CACHE_H

#include "runtime/linqu_orchestration_api.h"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <functional>

namespace linqu {

using LinquOrchEntryFn = void(*)(LinquRuntime* rt, uint64_t* args, int arg_count);
using LinquOrchConfigFn = LinquOrchConfig(*)(uint64_t* args, int arg_count);

struct LoadedKernel {
    void*             dl_handle = nullptr;
    LinquOrchEntryFn  entry_fn = nullptr;
    LinquOrchConfigFn config_fn = nullptr;
    std::string       so_path;
};

class CodeCache {
public:
    explicit CodeCache(const std::string& storage_path);

    void register_code(const std::string& name, const uint8_t* data, size_t len);

    bool has(const std::string& name) const;

    LoadedKernel load(const std::string& name);

    void unload_all();

private:
    std::string storage_path_;
    std::unordered_map<std::string, std::string> name_to_path_;
    std::unordered_map<std::string, LoadedKernel> loaded_;
};

}

#endif
