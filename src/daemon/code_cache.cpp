#include "daemon/code_cache.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>

namespace linqu {

static void mkdir_p_cc(const std::string& path) {
    std::string cur;
    for (size_t i = 0; i < path.size(); i++) {
        cur += path[i];
        if (path[i] == '/' || i == path.size() - 1) {
            mkdir(cur.c_str(), 0755);
        }
    }
}

CodeCache::CodeCache(const std::string& storage_path)
    : storage_path_(storage_path) {
    if (!storage_path.empty()) {
        mkdir_p_cc(storage_path);
        mkdir_p_cc(storage_path + "/code");
    }
}

void CodeCache::register_code(const std::string& name,
                               const uint8_t* data, size_t len) {
    std::string path = storage_path_ + "/code/" + name;
    std::ofstream f(path, std::ios::binary);
    if (f) {
        f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
        f.close();
        name_to_path_[name] = path;
    }
}

bool CodeCache::has(const std::string& name) const {
    return name_to_path_.count(name) > 0;
}

LoadedKernel CodeCache::load(const std::string& name) {
    auto it = loaded_.find(name);
    if (it != loaded_.end()) return it->second;

    auto pit = name_to_path_.find(name);
    if (pit == name_to_path_.end()) {
        return LoadedKernel{};
    }

    LoadedKernel k;
    k.so_path = pit->second;
    k.dl_handle = dlopen(k.so_path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!k.dl_handle) {
        fprintf(stderr, "[CodeCache] dlopen(%s) failed: %s\n",
                k.so_path.c_str(), dlerror());
        return LoadedKernel{};
    }

    k.entry_fn = reinterpret_cast<LinquOrchEntryFn>(
        dlsym(k.dl_handle, "linqu_orch_entry"));
    k.config_fn = reinterpret_cast<LinquOrchConfigFn>(
        dlsym(k.dl_handle, "linqu_orch_config"));

    if (!k.entry_fn) {
        fprintf(stderr, "[CodeCache] dlsym(linqu_orch_entry) failed for %s: %s\n",
                name.c_str(), dlerror());
        dlclose(k.dl_handle);
        return LoadedKernel{};
    }

    loaded_[name] = k;
    return k;
}

void CodeCache::unload_all() {
    for (auto& [name, k] : loaded_) {
        if (k.dl_handle) dlclose(k.dl_handle);
    }
    loaded_.clear();
}

}
