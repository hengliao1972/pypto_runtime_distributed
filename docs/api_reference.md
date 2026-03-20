# Linqu Runtime API Reference

## Core Types

### `linqu::Level` (src/core/level.h)
7-level hierarchy enum with 17 aliases:
- `CORE` (0), `CHIP_DIE` (1), `CHIP` (2), `HOST` (3), `CLUSTER_0`/`POD` (4), `CLUSTER_1`/`CLOS1` (5), `CLUSTER_2`/`CLOS2` (6)

### `linqu::LinquCoordinate` (src/core/coordinate.h)
7-field hierarchical address: `l6_idx`, `l5_idx`, `l4_idx`, `l3_idx`, `l2_idx`, `l1_idx`, `l0_idx`.
- `to_string()` → `(l6=0,l5=2,l4=1,l3=7,...)`
- `to_path()` → `L6_0/L5_2/L4_1/L3_7/...`
- `from_env()` → reads `LINQU_L0`–`LINQU_L6` environment variables
- Supports `==`, `!=`, `<` for ordering

### `linqu::TaskKey` (src/core/task_key.h)
Full task identity: `logical_system` (string), `coord` (LinquCoordinate), `scope_depth`, `task_id`. Hashable via `std::hash<TaskKey>`.

---

## Orchestration API (src/runtime/linqu_orchestration_api.h)

The **single header** included by every `.so` kernel at any level. Pure C API with C++ convenience wrappers.

### `LinquRuntimeOps` — Unified ops table

| Function | Signature | Description |
|----------|-----------|-------------|
| `submit_task` | `(rt, target, kernel_so, params, n)` | Dispatch kernel to target node |
| `scope_begin` | `(rt)` | Enter a new scope |
| `scope_end` | `(rt)` | Exit current scope, retire ring slots |
| `alloc_tensor` | `(rt, target, size) → handle` | Allocate buffer on ring |
| `free_tensor` | `(rt, handle)` | Early release (pl.free) |
| `orchestration_done` | `(rt)` | Signal orchestration complete |
| `reg_data` | `(rt, target, data, size) → handle` | Register existing data |
| `query_peers` | `(rt, level) → LinquPeerList` | Get peers at given level |
| `self_coord` | `(rt) → LinquCoordinate_C` | This node's coordinate |
| `wait_all` | `(rt)` | Wait for all dispatched tasks |
| `dump_ring_snapshot` | `(rt, label)` | Capture ring state for profiling |
| `log_*` | `(rt, fmt, ...)` | Logging (error/warn/info/debug) |
| `submit_task_group` | `(rt, kernel_so, group_params, n_gp, sub_tasks, n_sub)` | Submit N sub-tasks as one dependency-graph node |

### `LinquOrchConfig`
Every `.so` exports `linqu_orch_config()` returning: `level` (uint8_t), `expected_arg_count` (int).

### Function role model (`orchestrator` / `worker`)
- Grammar-level extension: `@pl.function(..., role=...)` and `pl.at(..., role=...)`.
- `orchestrator`: submits tasks and manages DAG/ring ownership.
- `worker`: executes concrete task payload and can dispatch lower-level orchestrators via runtime APIs.
- ABI note: current exported `LinquOrchConfig` stays unchanged for compatibility.

### `LINQU_SCOPE(rt)`
RAII scope guard macro. Usage: `LINQU_SCOPE(rt) { ... }` — automatically calls `scope_begin`/`scope_end`.

### `LinquParam`
Parameter descriptor: `type` (INPUT/OUTPUT/INOUT/SCALAR), `handle`, `scalar_value`. Convenience constructors: `linqu_make_input()`, `linqu_make_output()`, etc.

### `LinquSubTaskSpec`
Describes one sub-task within a group submission:
```c
typedef struct LinquSubTaskSpec {
    LinquCoordinate_C target;
    const char*       kernel_so;          /* NULL = use group-level kernel (SPMD) */
    LinquParam*       private_params;     /* per-target params (may be NULL) */
    int               num_private_params;
} LinquSubTaskSpec;
```

### `submit_task_group` — Group Task API

Submits N sub-tasks as a single node in the dependency graph. The group occupies one TaskRing slot; downstream tasks that depend on the group's OUTPUT tensors will only become ready after **all** sub-tasks complete.

```c
void linqu_submit_task_group(LinquRuntime* rt,
                              const char* kernel_so,        // group-level kernel (SPMD default)
                              LinquParam* group_params,     // shared INPUT/OUTPUT for dep tracking
                              int num_group_params,
                              LinquSubTaskSpec* sub_tasks,   // per-target sub-task specs
                              int num_sub_tasks);
```

Key semantics:
- **Dependency tracking**: `group_params` INPUT/OUTPUT handles build edges in the DAG, same as `submit_task`.
- **SPMD mode**: If a sub-task's `kernel_so` is NULL, the group-level kernel is used.
- **Private params**: Each sub-task can carry per-target params (e.g., shard_id), merged with group_params at dispatch time.
- **Empty group**: `num_sub_tasks == 0` immediately completes.
- **Sub-task IDs**: Synthetic negative IDs are used internally; they do not occupy TaskRing slots.

### `.so` Export Contract
```cpp
extern "C" {
    LinquOrchConfig linqu_orch_config(uint64_t* args, int arg_count);
    void linqu_orch_entry(LinquRuntime* rt, uint64_t* args, int arg_count);
}
```

---

## LinquOrchestratorState (src/runtime/linqu_orchestrator_state.h)

The central, unified, level-parameterized runtime implementation. One class serves all levels.

```cpp
void init(Level level, const LinquCoordinate& coord, const LinquOrchConfig_Internal& cfg);
void reset();
void submit_task(LinquCoordinate_C target, const char* kernel_so, LinquParam* params, int n);
void submit_task_group(const char* kernel_so, LinquParam* group_params, int num_group_params,
                       LinquSubTaskSpec* sub_tasks, int num_sub_tasks);
void scope_begin();
void scope_end();
uint64_t alloc_tensor(LinquCoordinate_C target, size_t size_bytes);
void free_tensor(uint64_t handle);
void wait_all();
LinquRuntime* runtime();       // Get opaque runtime pointer for kernel calls
void set_dispatcher(LinquDispatcher* d);
RingMetrics current_metrics();
ProfileReport generate_profile(const std::string& node_id);
void take_snapshot(const std::string& label);
```

---

## Dispatchers (src/runtime/)

### `LinquDispatcher` (Abstract)
```cpp
virtual bool dispatch(int32_t task_id, const LinquCoordinate& target,
                      const std::string& kernel_so, LinquParam* params, int n) = 0;
virtual void wait_all() = 0;
```

### `MockDispatcher`
Records dispatched tasks for testing. No actual execution.

### `RemoteDispatcher`
Sends `CALL_TASK` messages via `UnixSocketTransport` to remote daemon processes. Receives `TASK_COMPLETE` on a background thread.

---

## Transport (src/transport/)

### `LinquHeader` (24 bytes)
Binary wire format: magic (0x4C51524D), version, msg_type, sender/target coordinates, payload_size.

### `MsgType`
HEARTBEAT, REG_CODE, REG_DATA, CALL_TASK, SCOPE_EXIT, RETRY_WITH_CODE, TASK_COMPLETE, SHUTDOWN.

### Payload Structs
- `CallTaskPayload`: kernel name, task_id, scope_depth, params
- `TaskCompletePayload`: task_id, status
- `ShutdownPayload`: graceful flag

### `UnixSocketTransport`
Unix domain socket IPC. `make_socket_path(base, coord, level)` generates deterministic socket paths.

---

## Discovery (src/discovery/)

### `PeerRegistry`
Thread-safe membership table. `add()`, `remove()`, `set_alive()`, `peers_at_level()`, `peers_same_parent()`.

### `FilesystemDiscovery`
Scans filesystem for `daemon_L*.sock` files, parses coordinates from directory paths, populates PeerRegistry.

---

## Daemon (src/daemon/)

### `NodeDaemon`
Per-process daemon: listens on Unix socket, handles messages, loads `.so` kernels via `CodeCache`, creates `LinquOrchestratorState`, executes kernels. Writes `ring_profile.json` on shutdown.

### `CodeCache`
Stores and loads `.so` binaries. `register_code(name, data, len)`, `load(name) → LoadedKernel`.

### `NodeStoragePaths`
`create_node_storage(base, coord)` creates the standard directory hierarchy (node_dir, code_cache, data_cache, logs).

---

## Profiling (src/profiling/)

### `RingMetrics`
Per-(level, depth) counters: task_ring_capacity, peak_used, alloc/retire/block counts, buffer stats, scope counts, free_tensor counts.

### `RingSnapshot`
Point-in-time capture of ring state with label and microsecond timestamp.

### `ProfileReport`
Aggregates metrics and snapshots. `to_json()` produces human-readable JSON.
