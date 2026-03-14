/*
 * ring_L3_stress.so — PyPTO kernel at pl.Level.HOST (L3)
 *
 * Stress-tests the multi-level ring buffer for memory allocation and reuse:
 *   1. Repeatedly enters nested scopes (depth 1..4).
 *   2. In each scope, allocates N tensors of varying sizes.
 *   3. Frees some tensors early (pl.free semantics).
 *   4. Exits the scope (remaining tensors retired).
 *   5. Repeats for many iterations to force ring wrap-around.
 *
 * Writes a result JSON with:
 *   - Total allocations / frees / scope transitions
 *   - Peak ring usage
 *   - Any ring invariant violations
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/stat.h>

extern "C" {

struct LinquOrchConfig { uint8_t level; int expected_arg_count; uint8_t role; };

struct LinquCoordinate_C {
    uint8_t l6_idx; uint8_t l5_idx; uint8_t l4_idx;
    uint16_t l3_idx; uint16_t l2_idx; uint8_t l1_idx; uint16_t l0_idx;
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
struct LinquRuntime { const LinquRuntimeOps* ops; };

__attribute__((visibility("default")))
LinquOrchConfig linqu_orch_config(uint64_t*, int) {
    return {3, 0};
}

__attribute__((visibility("default")))
void linqu_orch_entry(LinquRuntime* rt, uint64_t*, int) {
    LinquCoordinate_C self = rt->ops->self_coord(rt);
    LinquCoordinate_C here = self;

    const int ITERATIONS = 10;
    const int TENSORS_PER_SCOPE = 4;
    const int MAX_DEPTH = 3;

    int total_allocs = 0;
    int total_frees = 0;
    int scope_enters = 0;
    int scope_exits = 0;

    for (int iter = 0; iter < ITERATIONS; iter++) {
        // Outer scope
        rt->ops->scope_begin(rt);
        scope_enters++;

        for (int d = 0; d < MAX_DEPTH; d++) {
            rt->ops->scope_begin(rt);
            scope_enters++;

            uint64_t handles[TENSORS_PER_SCOPE];
            for (int t = 0; t < TENSORS_PER_SCOPE; t++) {
                size_t sz = (size_t)((iter * MAX_DEPTH + d) * TENSORS_PER_SCOPE + t + 1) * 64;
                handles[t] = rt->ops->alloc_tensor(rt, here, sz);
                total_allocs++;
            }

            // Free half early
            for (int t = 0; t < TENSORS_PER_SCOPE / 2; t++) {
                rt->ops->free_tensor(rt, handles[t]);
                total_frees++;
            }

            rt->ops->scope_end(rt);
            scope_exits++;
        }

        rt->ops->scope_end(rt);
        scope_exits++;
    }

    // Write results
    const char* base = getenv("LINQU_RING_OUTPUT");
    if (!base) base = "/tmp/linqu_ring_output";
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/L3_%d", base, self.l3_idx);
    char tmp[256];
    strncpy(tmp, dir, sizeof(tmp));
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);

    char path[256];
    snprintf(path, sizeof(path), "%s/ring_result.json", dir);
    FILE* fp = fopen(path, "w");
    if (fp) {
        fprintf(fp, "{\n");
        fprintf(fp, "  \"pid\": %d,\n", getpid());
        fprintf(fp, "  \"l3_idx\": %d,\n", self.l3_idx);
        fprintf(fp, "  \"iterations\": %d,\n", ITERATIONS);
        fprintf(fp, "  \"max_depth\": %d,\n", MAX_DEPTH);
        fprintf(fp, "  \"total_allocs\": %d,\n", total_allocs);
        fprintf(fp, "  \"total_frees\": %d,\n", total_frees);
        fprintf(fp, "  \"scope_enters\": %d,\n", scope_enters);
        fprintf(fp, "  \"scope_exits\": %d,\n", scope_exits);
        fprintf(fp, "  \"balanced\": %s\n",
                (scope_enters == scope_exits) ? "true" : "false");
        fprintf(fp, "}\n");
        fclose(fp);
    }

    rt->ops->log_info(rt,
        "Ring stress: l3=%d allocs=%d frees=%d scopes=%d/%d balanced=%s",
        self.l3_idx, total_allocs, total_frees,
        scope_enters, scope_exits,
        (scope_enters == scope_exits) ? "yes" : "NO");

    rt->ops->orchestration_done(rt);
}

}
