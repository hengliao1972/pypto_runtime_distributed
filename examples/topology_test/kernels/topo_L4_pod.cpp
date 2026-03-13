/*
 * topo_L4_pod.so — L4 (POD) topology kernel
 * Discovers L3 host peers, dispatches topo_L3_host.so to each.
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>

extern "C" {

struct LinquOrchConfig { uint8_t level; int expected_arg_count; };
struct LinquCoordinate_C {
    uint8_t l6_idx; uint8_t l5_idx; uint8_t l4_idx;
    uint16_t l3_idx; uint16_t l2_idx; uint8_t l1_idx; uint16_t l0_idx;
};
struct LinquPeerList { LinquCoordinate_C* peers; int count; };

enum { LINQU_PARAM_INPUT = 0, LINQU_PARAM_OUTPUT = 1, LINQU_PARAM_INOUT = 2, LINQU_PARAM_SCALAR = 3 };
struct LinquParam { int type; uint64_t handle; uint64_t scalar_value; };

struct LinquRuntime;
struct LinquRuntimeOps {
    void (*submit_task)(LinquRuntime*, LinquCoordinate_C, const char*, LinquParam*, int);
    void (*scope_begin)(LinquRuntime*);
    void (*scope_end)(LinquRuntime*);
    uint64_t (*alloc_tensor)(LinquRuntime*, LinquCoordinate_C, size_t);
    void (*free_tensor)(LinquRuntime*, uint64_t);
    void (*orchestration_done)(LinquRuntime*);
    uint64_t (*reg_data)(LinquRuntime*, LinquCoordinate_C, const void*, size_t);
    LinquPeerList (*query_peers)(LinquRuntime*, uint8_t);
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
    return {4, 0};
}

__attribute__((visibility("default")))
void linqu_orch_entry(LinquRuntime* rt, uint64_t*, int) {
    LinquPeerList peers = rt->ops->query_peers(rt, 3);

    rt->ops->scope_begin(rt);
    for (int i = 0; i < peers.count; i++) {
        rt->ops->submit_task(rt, peers.peers[i], "topo_L3_host.so", nullptr, 0);
    }
    rt->ops->scope_end(rt);

    rt->ops->wait_all(rt);
    rt->ops->log_info(rt, "L4 POD: dispatched topo_L3_host to %d hosts", peers.count);
    if (peers.peers) free(peers.peers);
    rt->ops->orchestration_done(rt);
}

}
