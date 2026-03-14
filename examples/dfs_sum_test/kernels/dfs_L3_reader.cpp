/*
 * dfs_L3_reader.so — PyPTO kernel at pl.Level.HOST (L3), role = WORKER
 *
 * Represents the worker function in the lingqu_dfs hierarchical sum test.
 * Each L3 node reads its own DFS data file (shared namespace under
 * LINQU_DFS_BASE/L3_<idx>/data.txt), computes the local sum, and writes
 * the result to LINQU_DFS_OUTPUT/L3_<idx>/sum.json.
 *
 * PyPTO grammar equivalent:
 *   @pl.function(level=pl.Level.HOST, role=pl.Role.WORKER)
 *   def dfs_reader(dfs_base: str, output_base: str):
 *       data = lingqu_dfs.open(f"{dfs_base}/L3_{self.l3_idx}/data.txt")
 *       local_sum = sum(data)
 *       write_result(output_base, local_sum)
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

extern "C" {

/* --- Local struct definitions (no link-time deps) --- */

struct LinquOrchConfig { uint8_t level; int expected_arg_count; uint8_t role; };

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
    void     (*submit_task)(LinquRuntime*, LinquCoordinate_C, const char*, void*, int);
    void     (*scope_begin)(LinquRuntime*);
    void     (*scope_end)(LinquRuntime*);
    uint64_t (*alloc_tensor)(LinquRuntime*, LinquCoordinate_C, size_t);
    void     (*free_tensor)(LinquRuntime*, uint64_t);
    void     (*orchestration_done)(LinquRuntime*);
    uint64_t (*reg_data)(LinquRuntime*, LinquCoordinate_C, const void*, size_t);
    void*    (*query_peers)(LinquRuntime*, uint8_t);
    LinquCoordinate_C (*self_coord)(LinquRuntime*);
    void     (*wait_all)(LinquRuntime*);
    void     (*dump_ring_snapshot)(LinquRuntime*, const char*);
    void     (*log_error)(LinquRuntime*, const char*, ...);
    void     (*log_warn)(LinquRuntime*, const char*, ...);
    void     (*log_info)(LinquRuntime*, const char*, ...);
    void     (*log_debug)(LinquRuntime*, const char*, ...);
    void*    (*get_tensor_pool)(LinquRuntime*);
};
struct LinquRuntime { const LinquRuntimeOps* ops; };

/* --- Helpers --- */

static void mkdir_p(const char* path) {
    char tmp[512];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

/* --- Export: config ------------------------------------------------- */

__attribute__((visibility("default")))
LinquOrchConfig linqu_orch_config(uint64_t*, int) {
    /* level=3 (HOST), role=1 (WORKER) */
    LinquOrchConfig cfg;
    cfg.level = 3;
    cfg.expected_arg_count = 0;
    cfg.role = 1; /* LINQU_ROLE_WORKER */
    return cfg;
}

/* --- Export: entry -------------------------------------------------- */

__attribute__((visibility("default")))
void linqu_orch_entry(LinquRuntime* rt, uint64_t*, int) {
    LinquCoordinate_C self = rt->ops->self_coord(rt);

    /* Global L3 index: used to locate the DFS data file */
    const int global_l3 =
        (int)self.l6_idx * 1024 +
        (int)self.l5_idx * 64  +
        (int)self.l4_idx * 16  +
        (int)self.l3_idx;

    /* --- Read lingqu_dfs data file --- */
    const char* dfs_base = getenv("LINQU_DFS_BASE");
    if (!dfs_base) dfs_base = "/tmp/lingqu_dfs";

    char data_path[512];
    snprintf(data_path, sizeof(data_path), "%s/L3_%d/data.txt", dfs_base, global_l3);

    FILE* fp = fopen(data_path, "r");
    uint64_t local_sum = 0;
    int count = 0;
    if (fp) {
        int v;
        while (fscanf(fp, "%d", &v) == 1) {
            local_sum += (uint64_t)v;
            count++;
        }
        fclose(fp);
    } else {
        rt->ops->log_error(rt, "dfs_L3_reader: cannot open %s", data_path);
    }

    /* --- Write sum.json to output path --- */
    const char* out_base = getenv("LINQU_DFS_OUTPUT");
    if (!out_base) out_base = "/tmp/lingqu_dfs_output";

    char out_dir[512];
    snprintf(out_dir, sizeof(out_dir), "%s/L3_%d", out_base, global_l3);
    mkdir_p(out_dir);

    char out_path[512];
    snprintf(out_path, sizeof(out_path), "%s/sum.json", out_dir);
    FILE* of = fopen(out_path, "w");
    if (of) {
        fprintf(of, "{\n");
        fprintf(of, "  \"global_l3\": %d,\n", global_l3);
        fprintf(of, "  \"l3_idx\": %d,\n", (int)self.l3_idx);
        fprintf(of, "  \"l4_idx\": %d,\n", (int)self.l4_idx);
        fprintf(of, "  \"l5_idx\": %d,\n", (int)self.l5_idx);
        fprintf(of, "  \"count\": %d,\n", count);
        fprintf(of, "  \"local_sum\": %llu\n",
                (unsigned long long)local_sum);
        fprintf(of, "}\n");
        fclose(of);
    }

    rt->ops->log_info(rt,
        "dfs_L3_reader: global_l3=%d count=%d local_sum=%llu",
        global_l3, count, (unsigned long long)local_sum);

    rt->ops->orchestration_done(rt);
}

} /* extern "C" */
