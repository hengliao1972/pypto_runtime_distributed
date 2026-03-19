# Compiler Output Convention

## Overview

All orchestration functions — at every level L3–L6 — are compiled into standalone `.so` shared libraries with `extern "C"` entry points. The PyPTO compiler generates these; for testing, they are hand-written.

## Export Contract

Every `.so` file exports exactly two functions:

```c
extern "C" {
    __attribute__((visibility("default")))
    LinquOrchConfig linqu_orch_config(uint64_t* args, int arg_count);

    __attribute__((visibility("default")))
    void linqu_orch_entry(LinquRuntime* rt, uint64_t* args, int arg_count);
}
```

### `linqu_orch_config`
Returns a `LinquOrchConfig` struct:
- `level` — the hierarchy level this function runs at (3=HOST, 4=POD, 5=CLOS1, 6=CLOS2)
- `expected_arg_count` — number of arguments expected

### Role annotation (`orchestrator` vs `worker`)

Source-level grammar supports a function role concept:
- `@pl.function(..., role=pl.Role.ORCHESTRATOR | pl.Role.WORKER)`
- `pl.at(..., role=pl.Role.ORCHESTRATOR | pl.Role.WORKER)`

Current ABI remains unchanged (`LinquOrchConfig` still exports `level` + `expected_arg_count`).
Role is treated as compiler/runtime semantic metadata:
- `ORCHESTRATOR` functions build/submit DAG tasks.
- `WORKER` functions execute concrete tasks and may invoke lower-level orchestrators through runtime dispatch.

### `linqu_orch_entry`
The actual orchestration function. Receives:
- `rt` — opaque runtime pointer; all operations go through `rt->ops->...`
- `args` — array of `uint64_t` arguments (tensor handles and scalars)
- `arg_count` — number of arguments

## Header Dependency

Each `.so` includes only `linqu_orchestration_api.h`. This header defines:
- `LinquRuntime`, `LinquRuntimeOps` — the ops table
- `LinquParam`, `LinquParamType` — parameter types
- `LinquCoordinate_C` — coordinate struct
- `LinquPeerList` — peer discovery result
- `LinquSubTaskSpec` — sub-task spec for group submissions
- `LINQU_SCOPE(rt)` — RAII scope macro
- Inline wrappers: `linqu_submit_task`, `linqu_submit_task_group`, `linqu_scope_begin`, etc.

## Zero Link Dependencies

The `.so` has **no link-time dependencies** on the Linqu runtime. All calls go through the function-pointer table (`LinquRuntimeOps`), which is populated by the daemon before calling `linqu_orch_entry`.

## Level Tagging

The `level` field in `LinquOrchConfig` tells the daemon which level this function targets:

| Level | Constant | Dispatcher |
|-------|----------|------------|
| 3 (HOST) | `LINQU_LEVEL_HOST` | `LocalDispatcher` (stub to L2) |
| 4 (POD) | `LINQU_LEVEL_POD` | `RemoteDispatcher` → L3 daemons |
| 5 (CLOS1) | `LINQU_LEVEL_CLOS1` | `RemoteDispatcher` → L4 daemons |
| 6 (CLOS2) | `LINQU_LEVEL_CLOS2` | `RemoteDispatcher` → L5 daemons |

## Parameter Conventions

Parameters use `LinquParam` with four types:

| Type | Meaning | handle field | scalar_value field |
|------|---------|--------------|-------------------|
| `LINQU_PARAM_INPUT` | Read-only tensor | tensor handle | unused |
| `LINQU_PARAM_OUTPUT` | Write-only tensor | tensor handle | unused |
| `LINQU_PARAM_INOUT` | Read-write tensor | tensor handle | unused |
| `LINQU_PARAM_SCALAR` | Scalar constant | unused | the value |

## Example `.so` (L4 Pod Orchestration)

```cpp
#include "linqu_orchestration_api.h"

extern "C" {

LinquOrchConfig linqu_orch_config(uint64_t*, int) {
    return LinquOrchConfig{LINQU_LEVEL_POD, 0};
}

void linqu_orch_entry(LinquRuntime* rt, uint64_t* args, int arg_count) {
    LinquPeerList peers = linqu_query_peers(rt, LINQU_LEVEL_HOST);
    LINQU_SCOPE(rt) {
        for (int i = 0; i < peers.count; i++) {
            linqu_submit_task(rt, peers.peers[i], "my_L3_kernel.so", nullptr, 0);
        }
    }
    linqu_wait_all(rt);
    if (peers.peers) free(peers.peers);
    linqu_orchestration_done(rt);
}

}
```

## Group Task Convention

When the compiler emits a multi-device operation (e.g., two L2 chips working as one logical worker), it generates a `linqu_submit_task_group` call instead of multiple `linqu_submit_task` calls. This ensures the N sub-tasks form a single node in the dependency graph.

### Example: L3 Host dispatching to 2 chips as a group

```cpp
// Inside linqu_orch_entry of an L3 kernel:
LinquPeerList chips = linqu_query_peers(rt, LINQU_LEVEL_CHIP);

uint64_t input_buf  = args[0];
uint64_t output_buf = linqu_alloc_tensor(rt, chips.peers[0], 4096);

// Group params: shared INPUT/OUTPUT for dependency tracking
LinquParam gp[2] = {
    linqu_make_input(input_buf),
    linqu_make_output(output_buf),
};

// Per-chip private params (e.g., shard_id)
LinquParam p0 = linqu_make_scalar(0);
LinquParam p1 = linqu_make_scalar(1);

LinquSubTaskSpec subs[2] = {
    { chips.peers[0], "compute.so", &p0, 1 },
    { chips.peers[1], nullptr,      &p1, 1 },  // NULL kernel = SPMD (use group kernel)
};

linqu_submit_task_group(rt, "compute.so", gp, 2, subs, 2);

// Subsequent tasks depending on output_buf automatically depend on the entire group
linqu_submit_task(rt, chips.peers[0], "next.so",
                  (LinquParam[]){ linqu_make_input(output_buf) }, 1);
```

The compiler should emit `submit_task_group` when:
- A `pl.at()` call targets multiple devices that share input/output tensors
- The source uses `pl.group()` or equivalent multi-device dispatch syntax
- SPMD patterns where the same kernel runs on N targets with different shard parameters
