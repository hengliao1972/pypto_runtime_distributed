#ifndef LINQU_ORCHESTRATION_API_H
#define LINQU_ORCHESTRATION_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ====================================================================
 * Level Constants (same as pl.Level)
 * ==================================================================== */
enum {
    LINQU_LEVEL_CORE      = 0,
    LINQU_LEVEL_CHIP_DIE  = 1,
    LINQU_LEVEL_CHIP      = 2,
    LINQU_LEVEL_HOST      = 3,
    LINQU_LEVEL_POD       = 4,
    LINQU_LEVEL_CLOS1     = 5,
    LINQU_LEVEL_CLOS2     = 6,
};

/* ====================================================================
 * LinquCoordinate (C-compatible 7-level hierarchical address)
 * ==================================================================== */
typedef struct LinquCoordinate_C {
    uint8_t  l6_idx;
    uint8_t  l5_idx;
    uint8_t  l4_idx;
    uint16_t l3_idx;
    uint16_t l2_idx;
    uint8_t  l1_idx;
    uint16_t l0_idx;
} LinquCoordinate_C;

/* ====================================================================
 * LinquParam — mirrors PTOParam semantics
 * ==================================================================== */
typedef enum {
    LINQU_PARAM_INPUT  = 0,
    LINQU_PARAM_OUTPUT = 1,
    LINQU_PARAM_INOUT  = 2,
    LINQU_PARAM_SCALAR = 3,
} LinquParamType;

typedef struct LinquParam {
    LinquParamType type;
    uint64_t handle;
    uint64_t scalar_value;
} LinquParam;

static inline LinquParam linqu_make_input(uint64_t h) {
    LinquParam p; p.type = LINQU_PARAM_INPUT; p.handle = h; p.scalar_value = 0; return p;
}
static inline LinquParam linqu_make_output(uint64_t h) {
    LinquParam p; p.type = LINQU_PARAM_OUTPUT; p.handle = h; p.scalar_value = 0; return p;
}
static inline LinquParam linqu_make_inout(uint64_t h) {
    LinquParam p; p.type = LINQU_PARAM_INOUT; p.handle = h; p.scalar_value = 0; return p;
}
static inline LinquParam linqu_make_scalar(uint64_t v) {
    LinquParam p; p.type = LINQU_PARAM_SCALAR; p.handle = 0; p.scalar_value = v; return p;
}

/* ====================================================================
 * LinquPeerList — returned by query_peers
 * ==================================================================== */
typedef struct LinquPeerList {
    LinquCoordinate_C* peers;
    int count;
} LinquPeerList;

/* ====================================================================
 * Forward declare
 * ==================================================================== */
typedef struct LinquRuntime LinquRuntime;

/* ====================================================================
 * LinquRuntimeOps — unified ops table for ALL levels (L0–L6)
 *
 * Mirrors PTO2RuntimeOps. The orchestration .so has zero link deps;
 * all calls go through this function-pointer table.
 *
 * At L0–L2 the runtime wraps simpler's implementation behind these
 * same function pointers. At L3–L6 LinquOrchestratorState provides
 * the implementation. The orchestration function cannot tell the
 * difference.
 * ==================================================================== */
typedef struct LinquRuntimeOps {
    void (*submit_task)(LinquRuntime* rt,
                        LinquCoordinate_C target,
                        const char* kernel_so,
                        LinquParam* params, int num_params);

    void (*scope_begin)(LinquRuntime* rt);
    void (*scope_end)(LinquRuntime* rt);

    uint64_t (*alloc_tensor)(LinquRuntime* rt,
                             LinquCoordinate_C target,
                             size_t size_bytes);
    void (*free_tensor)(LinquRuntime* rt, uint64_t handle);

    void (*orchestration_done)(LinquRuntime* rt);

    uint64_t (*reg_data)(LinquRuntime* rt,
                         LinquCoordinate_C target,
                         const void* data, size_t size);

    LinquPeerList (*query_peers)(LinquRuntime* rt, uint8_t level);

    LinquCoordinate_C (*self_coord)(LinquRuntime* rt);

    void (*wait_all)(LinquRuntime* rt);

    void (*dump_ring_snapshot)(LinquRuntime* rt, const char* label);

    void (*log_error)(LinquRuntime* rt, const char* fmt, ...);
    void (*log_warn)(LinquRuntime* rt, const char* fmt, ...);
    void (*log_info)(LinquRuntime* rt, const char* fmt, ...);
    void (*log_debug)(LinquRuntime* rt, const char* fmt, ...);

    void* (*get_tensor_pool)(LinquRuntime* rt);
} LinquRuntimeOps;

/* ====================================================================
 * LinquRuntime — opaque runtime pointer (same pattern as PTO2Runtime)
 * ==================================================================== */
struct LinquRuntime {
    const LinquRuntimeOps* ops;
};

/* ====================================================================
 * LinquOrchConfig — every .so exports this via linqu_orch_config()
 * ==================================================================== */
typedef struct LinquOrchConfig {
    uint8_t level;
    int expected_arg_count;
} LinquOrchConfig;

/* ====================================================================
 * .so export contract (same signature for ALL levels):
 *
 *   extern "C" {
 *     __attribute__((visibility("default")))
 *     LinquOrchConfig linqu_orch_config(uint64_t* args, int arg_count);
 *
 *     __attribute__((visibility("default")))
 *     void linqu_orch_entry(LinquRuntime* rt, uint64_t* args, int arg_count);
 *   }
 * ==================================================================== */

/* ====================================================================
 * Inline convenience wrappers
 * ==================================================================== */
static inline void linqu_submit_task(LinquRuntime* rt, LinquCoordinate_C target,
                                     const char* kernel_so,
                                     LinquParam* params, int num_params) {
    rt->ops->submit_task(rt, target, kernel_so, params, num_params);
}
static inline void linqu_scope_begin(LinquRuntime* rt) {
    rt->ops->scope_begin(rt);
}
static inline void linqu_scope_end(LinquRuntime* rt) {
    rt->ops->scope_end(rt);
}
static inline uint64_t linqu_alloc_tensor(LinquRuntime* rt,
                                           LinquCoordinate_C target,
                                           size_t size_bytes) {
    return rt->ops->alloc_tensor(rt, target, size_bytes);
}
static inline void linqu_free_tensor(LinquRuntime* rt, uint64_t handle) {
    rt->ops->free_tensor(rt, handle);
}
static inline void linqu_orchestration_done(LinquRuntime* rt) {
    rt->ops->orchestration_done(rt);
}
static inline uint64_t linqu_reg_data(LinquRuntime* rt,
                                       LinquCoordinate_C target,
                                       const void* data, size_t size) {
    return rt->ops->reg_data(rt, target, data, size);
}
static inline LinquPeerList linqu_query_peers(LinquRuntime* rt, uint8_t level) {
    return rt->ops->query_peers(rt, level);
}
static inline LinquCoordinate_C linqu_self_coord(LinquRuntime* rt) {
    return rt->ops->self_coord(rt);
}
static inline void linqu_wait_all(LinquRuntime* rt) {
    rt->ops->wait_all(rt);
}
static inline void linqu_dump_ring_snapshot(LinquRuntime* rt, const char* label) {
    rt->ops->dump_ring_snapshot(rt, label);
}

#ifdef __cplusplus
}  /* extern "C" */

/* ====================================================================
 * C++ RAII Scope Guard and LINQU_SCOPE macro
 * ==================================================================== */
class LinquScopeGuard {
public:
    explicit LinquScopeGuard(LinquRuntime* rt) : rt_(rt) {
        rt_->ops->scope_begin(rt_);
    }
    ~LinquScopeGuard() {
        rt_->ops->scope_end(rt_);
    }
    LinquScopeGuard(const LinquScopeGuard&) = delete;
    LinquScopeGuard& operator=(const LinquScopeGuard&) = delete;
private:
    LinquRuntime* rt_;
};

#define LINQU_CAT_IMPL_(x, y) x ## y
#define LINQU_CAT_(x, y) LINQU_CAT_IMPL_(x, y)
#define LINQU_SCOPE_GUARD(rt) \
    [[maybe_unused]] LinquScopeGuard LINQU_CAT_(lq_sg_, __COUNTER__)(rt)
#define LINQU_SCOPE(rt) if (LINQU_SCOPE_GUARD(rt); true)

/* ====================================================================
 * Logging macros
 * ==================================================================== */
#define LINQU_LOG_ERROR(rt, fmt, ...) (rt)->ops->log_error(rt, fmt, ##__VA_ARGS__)
#define LINQU_LOG_WARN(rt, fmt, ...)  (rt)->ops->log_warn(rt, fmt, ##__VA_ARGS__)
#define LINQU_LOG_INFO(rt, fmt, ...)  (rt)->ops->log_info(rt, fmt, ##__VA_ARGS__)
#define LINQU_LOG_DEBUG(rt, fmt, ...) (rt)->ops->log_debug(rt, fmt, ##__VA_ARGS__)

#endif /* __cplusplus */

#endif /* LINQU_ORCHESTRATION_API_H */
