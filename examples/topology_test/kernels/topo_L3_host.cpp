/*
 * topo_L3_host.so — PyPTO kernel at pl.Level.HOST (L3)
 *
 * This is what the PyPTO compiler would emit for:
 *   @pl.function(level=pl.Level.HOST)
 *   def host_identity():
 *       pl.dump_identity()
 *
 * When executed by a L3 daemon, it:
 *   1. Queries its own coordinate via the ops table.
 *   2. Writes identity.json to a well-known path.
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>

extern "C" {

struct LinquOrchConfig {
    uint8_t level;
    int     expected_arg_count;
    uint8_t role;
};

struct LinquCoordinate_C {
    uint8_t  l6_idx;
    uint8_t  l5_idx;
    uint8_t  l4_idx;
    uint16_t l3_idx;
    uint16_t l2_idx;
    uint8_t  l1_idx;
    uint16_t l0_idx;
};

struct LinquRuntime;

struct LinquRuntimeOps {
    void (*submit_task)(LinquRuntime*, LinquCoordinate_C, const char*, void*, int);
    void (*scope_begin)(LinquRuntime*);
    void (*scope_end)(LinquRuntime*);
    uint64_t (*alloc_tensor)(LinquRuntime*, LinquCoordinate_C, size_t);
    void (*free_tensor)(LinquRuntime*, uint64_t);
    void (*orchestration_done)(LinquRuntime*);
    uint64_t (*reg_data)(LinquRuntime*, LinquCoordinate_C, const void*, size_t);
    void* (*query_peers)(LinquRuntime*, uint8_t);
    LinquCoordinate_C (*self_coord)(LinquRuntime*);
    void (*wait_all)(LinquRuntime*);
    void (*dump_ring_snapshot)(LinquRuntime*, const char*);
    void (*log_error)(LinquRuntime*, const char*, ...);
    void (*log_warn)(LinquRuntime*, const char*, ...);
    void (*log_info)(LinquRuntime*, const char*, ...);
    void (*log_debug)(LinquRuntime*, const char*, ...);
    void* (*get_tensor_pool)(LinquRuntime*);
};

struct LinquRuntime {
    const LinquRuntimeOps* ops;
};

__attribute__((visibility("default")))
LinquOrchConfig linqu_orch_config(uint64_t*, int) {
    return {3, 0};
}

__attribute__((visibility("default")))
void linqu_orch_entry(LinquRuntime* rt, uint64_t* args, int arg_count) {
    (void)args; (void)arg_count;

    LinquCoordinate_C self = rt->ops->self_coord(rt);
    int global_idx = self.l5_idx * 64 + self.l4_idx * 16 + self.l3_idx;
    pid_t pid = getpid();

    // Write identity to a well-known path under LINQU_BASE
    const char* base = getenv("LINQU_TOPO_OUTPUT");
    if (!base) base = "/tmp/linqu_topo_output";

    char dir[512];
    snprintf(dir, sizeof(dir), "%s/L6_%d/L5_%d/L4_%d/L3_%d",
             base, self.l6_idx, self.l5_idx, self.l4_idx, self.l3_idx);

    // mkdir -p
    char tmp[512];
    strncpy(tmp, dir, sizeof(tmp));
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);

    char path[512];
    snprintf(path, sizeof(path), "%s/identity.json", dir);
    FILE* f = fopen(path, "w");
    if (f) {
        fprintf(f, "{\n");
        fprintf(f, "  \"pid\": %d,\n", pid);
        fprintf(f, "  \"coordinate\": {\"l6\": %d, \"l5\": %d, \"l4\": %d, \"l3\": %d},\n",
                self.l6_idx, self.l5_idx, self.l4_idx, self.l3_idx);
        fprintf(f, "  \"global_index\": %d\n", global_idx);
        fprintf(f, "}\n");
        fclose(f);
    }

    if (rt->ops->log_info) {
        rt->ops->log_info(rt, "L3 identity: l5=%d l4=%d l3=%d global=%d pid=%d",
                          self.l5_idx, self.l4_idx, self.l3_idx, global_idx, pid);
    }

    rt->ops->orchestration_done(rt);
}

}
