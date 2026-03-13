# Linqu Distributed Runtime — Detailed Implementation Plan

This plan breaks down Phase 0 of the Linqu runtime design into small, concrete steps with specific goals, deliverables, and acceptance criteria. Phase 0 targets a **single-host (Level 3) environment** with all software forward-compatible with the full 7-layer system. All code lives within `pypto_runtime_distributed` — no dependency on or modification to `simpler`.

**Key Assumption:** A PyPTO front-end compiler already generates C++ functions tagged with hierarchy levels (L0–L6). For Levels 3–6, these functions use the Linqu runtime API (`LinquRuntime*`, `LinquRuntimeOps`, `LINQU_SCOPE`, `linqu_submit_task`). For Levels 0–2, `simpler` has its own API (`PTO2Runtime*`, `PTO2RuntimeOps`, `PTO2_SCOPE`, `pto2_rt_submit_task`). The Linqu runtime compiles L3–L6 functions into arm64 shared libraries (`.so`) and dispatches them to the appropriate hierarchy node for execution.

**Scope Boundary — `simpler` Is Not Modified or Linked:** The `simpler` runtime (Levels 0–2) is a **separate, immutable codebase** (`pypto_workspace/simpler/`). This project (`pypto_runtime_distributed`) does **NOT** link against `simpler`, does **NOT** include `simpler` headers, and does **NOT** call any `simpler` API. The two runtimes are completely independent codebases. Integration between Linqu (L3+) and `simpler` (L0–L2) is achieved at a well-defined **Tier 2 adapter boundary** (see §7.4 of `linqu_runtime_design.md`), which will be implemented in a future phase when actual chip hardware is available. In Phase 0, the L3 daemon's downward dispatch to L2 is **stubbed** — it simulates chip-level task execution without calling into `simpler`.

**Execution Model:** At every level, an orchestration function runs as a **single process** that submits tasks to **multiple instances at the next lower level**. For example, L4 (pod) is a single process that orchestrates work across many L3 hosts. The same pattern repeats at L5 (one process → many L4 pods) and L6 (one process → many L5 supernodes). The runtime API, ring buffers, TensorMap, and scope stack are identical at every level (L3–L6); only the dispatch transport differs. At the L3→L2 boundary, a future `ChipBackend` adapter will translate Linqu dispatch calls into `simpler` API calls with `h2d_copy`/`d2h_copy` for host↔device data transfer.

**Per-Level Process Isolation (Fundamental Constraint):** Every hierarchy level runs its orchestration function and runtime in a **dedicated, independent OS process**. Levels do NOT share address space. All cross-level communication goes through IPC (Unix domain sockets in the verification environment, TCP/RDMA in production). This means:

```
Process model for the 1024-node virtual cluster:

  1 × L6 process   — runs topo_L6_cluster.so   — dispatches via IPC to L5 processes
 16 × L5 processes — each runs topo_L5_supernode.so — dispatches via IPC to L4 processes
 64 × L4 processes — each runs topo_L4_pod.so   — dispatches via IPC to L3 processes
1024 × L3 processes — each runs topo_L3_host.so  — stub dispatch to L2 (future: ChipBackend → simpler)
─────────────────
Total: 1105 processes for the full cluster
```

When an L4 function calls `submit_task(target_L3, "kernel.so", params)`:
1. The L4 runtime serializes the task (kernel name, params) into a message.
2. Sends the message via Unix socket to the target L3 process.
3. The L3 process receives the message, `dlopen`s the kernel `.so`, creates its own `LinquOrchestratorState`, and executes the function.
4. The L3 process sends a completion message back to L4.
5. The L4 runtime marks the task as COMPLETED in its task ring.

This ensures:
- Each level has its **own** ring buffers, tensor map, and scope stack in its **own** address space.
- No pointer sharing across levels — only serialized tensor handles and data buffers cross the IPC boundary.
- A crash at one level does not corrupt another level's state.
- The same isolation model applies whether processes are on the same machine (verification) or different machines (production).

**Three-Tier Communication Architecture:** The hierarchy has three fundamentally different communication tiers, each with distinct memory models and synchronization mechanisms. Understanding this is critical for correct implementation:

```
┌─────────────────────────────────────────────────────────────────────┐
│  Tier 3: Message Passing (L4–L6 ↔ L3)                              │
│  Unix Socket / TCP / RDMA                                           │
│  Serialized messages: CALL_TASK, TASK_COMPLETE, SHUTDOWN            │
│  Each node: independent process, independent address space          │
├─────────────────────────────────────────────────────────────────────┤
│  Tier 2: Host-Device DMA (L3 ↔ L2)                                 │
│  h2d_copy / d2h_copy                                                │
│  L3 = Host CPU (host memory), L2 = Device (device GM)              │
│  Different memory domains — no shared address space                 │
│  Data transfer: explicit DMA between host memory and device GM      │
├─────────────────────────────────────────────────────────────────────┤
│  Tier 1: Shared Device GM (L0–L2, managed by simpler)               │
│  Atomic operations + memory barriers (LOAD_ACQUIRE / STORE_RELEASE) │
│  Orchestrator + Scheduler + Workers share same GM address space     │
│  Zero-copy: pointer passing via ring buffers in device GM           │
└─────────────────────────────────────────────────────────────────────┘
```

This has key implications for the Linqu runtime implementation:

1. **Linqu's `LinquOrchestratorState` (L3–L6) uses `std::mutex`, NOT atomics.** There is no shared memory between levels — synchronization is for thread safety (recv thread vs. main thread within one process), not cross-process coordination.
2. **Linqu's TensorMap uses opaque integer handles, NOT device GM addresses.** At L3–L6 there is no shared buffer space, so multi-dimensional overlap detection (which `simpler` uses on GM addresses) is unnecessary. Handle-based exact-match tracking is correct.
3. **The L3 NodeDaemon will bridge Tier 2 and Tier 3 (future phase).** It receives CALL_TASK from L4 via Unix socket (Tier 3). In Phase 0, L2 dispatch is stubbed. In a future phase, the `ChipBackend` adapter will call `h2d_copy` to push data to device GM (Tier 2), invoke `simpler` for chip execution, call `d2h_copy` to pull results back, and send TASK_COMPLETE to L4 (Tier 3). This adapter lives in `pypto_runtime_distributed` and calls `simpler` through a stable ABI — it does NOT modify `simpler`.
4. **Back-pressure at L3–L6 is message-driven.** When rings are full, the orchestrator spins waiting for `TASK_COMPLETE` messages (triggering `on_task_complete` → `propagate_completion` → `try_advance_ring_pointers`), not on `LOAD_ACQUIRE(last_task_alive)` in shared memory.

See `linqu_runtime_design.md` §7.4 for the full specification.

**All test programs are implemented as "pypto kernels"** — C/C++ functions that look exactly like what the PyPTO compiler would emit after processing a `pl.at(level=...)` program. We skip the PyPTO compilation step and hand-write the compiler output directly. Each function:
- Has a `level` tag (the hierarchy level it runs at).
- Uses the **unified ops-table API** (`LinquRuntime*` / `LinquRuntimeOps`) at all levels L0–L6.
- Is compiled to a standalone `.so` with zero link dependencies.
- Can call `submit_task` / `scope_begin` / `scope_end` / `free_tensor` through the ops table.
- Receives input/output tensor handles and scalar parameters, never raw pointers across nodes.

See **"Unified Runtime API"** (Milestone 1B) for the complete contract.

**Verification Environment:** Since multi-host cluster hardware is not yet available, each hierarchy node (L3 host, L4 pod, L5 supernode, L6 cluster) runs as a **separate process** on the same arm64 server machine. Each process has its own persistent storage folder on disk, named by its hierarchical coordinate. This creates a fully functional multi-level verification environment on a single physical machine with **1105 total processes** (1 L6 + 16 L5 + 64 L4 + 1024 L3).

Reference: `pypto_top_level_design_documents/linqu_runtime_design.md`

---

## Milestone 1: Project Scaffolding

**Goal:** Set up the repository structure and build system. This project is **self-contained** — it does NOT link against or include headers from `simpler`. All code lives within `pypto_runtime_distributed`.

### Step 1.1: Repository Structure

**Action:** Create the initial directory layout.

```
pypto_runtime_distributed/
├── docs/                     # Design docs and this plan
├── src/
│   ├── core/                 # Hierarchy model, identity, coordinates
│   ├── ring/                 # Multi-layer ring buffer implementation
│   ├── scope/                # ScopeManager for Level 3+
│   ├── runtime/              # LinquOrchestratorState, dispatchers
│   ├── transport/            # RPC protocol, message header, IPC
│   ├── discovery/            # PeerRegistry, gossip, coordinate mapping
│   ├── dispatch/             # pl.at() dispatch, SPMD fan-out
│   ├── daemon/               # Node daemon (per-process runtime)
│   └── profiling/            # Ring metrics, JSON output
├── tests/
│   ├── unit/                 # Per-module unit tests
│   ├── forward_compat/       # Mock 7-level topology tests
│   └── e2e/                  # End-to-end multi-process tests
├── examples/
│   └── hierarchical_vecadd/  # THE hierarchical test program
│       ├── L3_host_orch.cpp  # Host-level orchestration function
│       ├── L4_pod_orch.cpp   # Pod-level orchestration function
│       ├── L5_super_orch.cpp # Supernode-level orchestration function
│       ├── L6_cluster_orch.cpp # Cluster-level orchestration function
│       ├── CMakeLists.txt    # Build all levels into arm64 .so files
│       └── run_cluster.sh    # Launch all processes for the test
├── CMakeLists.txt
└── README.md
```

**Deliverable:** Empty directory structure with CMake build skeleton for arm64 (aarch64-linux-gnu).

**Acceptance:** `cmake .. && make` compiles an empty project with no errors on the arm64 machine.

### Step 1.2: Language and Build Decision

**Action:** C++. All orchestration functions compile into `.so` shared libraries with `extern "C"` entry points. The Linqu runtime loads them via `dlopen`/`dlsym`.

The CMake build system must:
- Cross-compile or natively compile for **aarch64** (arm64).
- Build as a **standalone project** — no dependency on `simpler` or any external runtime library.
- Produce per-level `.so` files for each orchestration function.
- Build the `linqu_daemon` executable (the node process).

**Deliverable:** `CMakeLists.txt` at project root, verified to compile on the arm64 server.

### Step 1.3: Document `simpler` API Surface (Read-Only Reference)

**Action:** Read `pypto_workspace/simpler/` source to **document** (not link against) the `simpler` API. This is a read-only study to understand the API that the future `ChipBackend` adapter will need to call. The output is a reference document — no code in `pypto_runtime_distributed` depends on it in Phase 0.

Key `simpler` APIs to document for future adapter work:

1. **`PTO2RuntimeOps` function-pointer table** — `submit_task`, `scope_begin`, `scope_end`, `orchestration_done`.
2. **`PTO2Runtime*`** — opaque runtime context.
3. **`aicpu_orchestration_entry(PTO2Runtime* rt, uint64_t* args, int arg_count)`** — standard entry signature.
4. **`PTO2SharedMemoryHandle`** — shared memory between host CPU and device.
5. **h2d/d2h DMA API** — mechanism for transferring data between host memory and device GM.

**Deliverable:** `docs/simpler_api_contract.md` — reference document for the future `ChipBackend` adapter.

**Acceptance:** A peer can read this document and understand the Tier 2 boundary that the future adapter must bridge. No code in `pypto_runtime_distributed` includes or links `simpler` headers/libraries.

---

## Milestone 1B: Unified Runtime API and Implementation — Same Code at Every Level

**Goal:** Design and implement a **single, level-parameterized runtime** that provides the same API to orchestration functions at every hierarchy level (L3–L6). The API follows the same ops-table pattern used by `simpler` (`PTO2Runtime` / `PTO2RuntimeOps`) but is an independent implementation. The same runtime implementation code is instantiated once per level — no separate code paths for L3 vs L4 vs L5 vs L6. L0–L2 execution is handled by `simpler` (a separate, unmodified codebase); integration between Linqu and `simpler` will be implemented in a future phase via a `ChipBackend` adapter that does NOT modify `simpler`.

### Fundamental Architectural Principle: Single-Process Orchestration over Multiple Lower-Level Instances

At every level of the hierarchy, the execution model is the same:

> **One orchestration process at level L submits tasks to many instances at level L−1.**

This is the universal, recursive pattern that holds from L6 down to L0:

```
L6 orchestration (1 process)  ──submit_task──→  16 × L5 instances (supernodes)
L5 orchestration (1 process)  ──submit_task──→   4 × L4 instances (pods)
L4 orchestration (1 process)  ──submit_task──→  16 × L3 instances (hosts)
L3 orchestration (1 process)  ──submit_task──→   N × L2 instances (chips)
L2 orchestration (1 process)  ──submit_task──→   M × L0 instances (cores/core-groups)
```

**L4 (Pod) is the key example**: An L4 orchestration function runs as a **single process** on one host. It calls `linqu_submit_task()` to dispatch work to **multiple L3 hosts** in the same pod. The orchestration process owns the ring buffers, tensor map, and scope stack; the L3 hosts are the workers that execute the submitted tasks.

This pattern repeats identically at every level:
- L3 host orchestration dispatches to L2 chips (future: via `ChipBackend` adapter calling `simpler`; Phase 0: stubbed).
- L4 pod orchestration dispatches to multiple L3 hosts in the same pod.
- L5 supernode orchestration dispatches to multiple L4 pods.
- L6 cluster orchestration dispatches to multiple L5 supernodes.

The runtime API is therefore always: **"I am a single orchestrator at level L. I call `submit_task(target, kernel, params)` to dispatch work to one of my child instances at level L−1. The runtime manages the ring buffers, dependency tracking (TensorMap), and scope lifetimes for my level."**

This is why the same `LinquOrchestratorState` code works at every level (L3–L6) — the logic is always: allocate task slot, look up input dependencies in TensorMap, register output in TensorMap, dispatch to child, track scope ownership. Only the dispatch mechanism changes (IPC for same-machine processes, network for cross-machine). The L3→L2 boundary is special (Tier 2: h2d_copy/d2h_copy) and will be handled by a future `ChipBackend` adapter.

### Design Principle: One Runtime for L3–L6, Inspired by `simpler`

`simpler` provides a mature runtime for L0–L2 with proven subsystems. The Linqu runtime implements **equivalent algorithms** for L3–L6, adapted to work with local host memory instead of device shared memory. The table below shows the correspondence (for reference only — Linqu does NOT link against `simpler`):

| Subsystem | simpler type (L0–L2, reference) | Linqu type (L3–L6, implemented) |
|-----------|------|------|
| Ops table | `PTO2RuntimeOps` | `LinquRuntimeOps` |
| Orchestrator state | `PTO2OrchestratorState` | `LinquOrchestratorState` |
| Task ring | `PTO2TaskRing` | `LinquTaskRing` |
| Heap ring | `PTO2HeapRing` | `LinquHeapRing` |
| Dep list pool | `PTO2DepListPool` | `LinquDepListPool` |
| TensorMap | `PTO2TensorMap` (address-based overlap) | `LinquTensorMap` (handle-based exact match) |
| Scope stack | Embedded in `PTO2OrchestratorState` | `LinquScopeStack` |
| Scheduler | `PTO2SchedulerState` (device-side, atomics) | Message-driven in `LinquOrchestratorState` (mutex, TASK_COMPLETE) |

The Linqu runtime provides the **identical set of subsystems** for L3–L6, with one critical generalization: **all subsystems are parameterized by `(level, depth)`** so a single `LinquOrchestratorState` instance can serve any level.

```
┌────────────────────────────────────────────────────────────────────┐
│                    LinquOrchestratorState                         │
│                    (one instance per active level)                 │
│                                                                    │
│  level: Level          ← which level this instance serves          │
│  coord: LinquCoordinate ← identity of this node                   │
│                                                                    │
│  ┌─────────────────────────────────────────────────────────┐      │
│  │ Ring Buffers (LinquHeapRing / LinquTaskRing)            │      │
│  │                                                         │      │
│  │  task_ring[MAX_SCOPE_DEPTH]     — per-depth task slots  │      │
│  │  buffer_ring[MAX_SCOPE_DEPTH]   — per-depth heap alloc  │      │
│  │  dep_pool                       — dependency list pool   │      │
│  └─────────────────────────────────────────────────────────┘      │
│                                                                    │
│  ┌─────────────────────────────────────────────────────────┐      │
│  │ TensorMap (LinquTensorMap — handle-based exact match)    │      │
│  │                                                         │      │
│  │  Hash-table + ring-buffer entry pool                    │      │
│  │  Lazy invalidation, per-task chains, INOUT optimization │      │
│  │  Opaque handles (not GM addresses — no overlap detect)  │      │
│  └─────────────────────────────────────────────────────────┘      │
│                                                                    │
│  ┌─────────────────────────────────────────────────────────┐      │
│  │ Scope Stack (LinquScopeStack)                            │      │
│  │                                                         │      │
│  │  scope_tasks[]       — flat buffer of task IDs          │      │
│  │  scope_begins[]      — per-scope start index            │      │
│  │  scope_stack_top     — current depth                    │      │
│  └─────────────────────────────────────────────────────────┘      │
│                                                                    │
│  ┌─────────────────────────────────────────────────────────┐      │
│  │ Scheduler / Dispatcher (message-driven)                  │      │
│  │                                                         │      │
│  │  L3:    local execution or stub to L2 (future: adapter) │      │
│  │  L4–L6: dispatches to child nodes via IPC/network       │      │
│  └─────────────────────────────────────────────────────────┘      │
│                                                                    │
│  statistics, profiling, logging                                    │
└────────────────────────────────────────────────────────────────────┘
```

### The Unified Ops Table

The orchestration `.so` at every level (L3–L6) sees the **same** ops table type (`LinquRuntimeOps`). This is the Linqu runtime's own API — it is inspired by `simpler`'s `PTO2RuntimeOps` pattern but is an independent type defined in `pypto_runtime_distributed`. L0–L2 functions use `simpler`'s own `PTO2RuntimeOps` in a separate codebase.

```cpp
// linqu_orchestration_api.h — THE SINGLE HEADER for all levels
// Included by every orchestration .so regardless of level

#ifndef LINQU_ORCHESTRATION_API_H
#define LINQU_ORCHESTRATION_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Forward declarations
typedef struct LinquRuntime LinquRuntime;

// =============================================================================
// Level Constants (same as pl.Level)
// =============================================================================
enum {
    LINQU_LEVEL_CORE      = 0,
    LINQU_LEVEL_CHIP_DIE  = 1,
    LINQU_LEVEL_CHIP      = 2,
    LINQU_LEVEL_HOST      = 3,
    LINQU_LEVEL_POD       = 4,
    LINQU_LEVEL_CLOS1     = 5,
    LINQU_LEVEL_CLOS2     = 6,
};

// =============================================================================
// LinquCoordinate (7-level hierarchical address)
// =============================================================================
typedef struct LinquCoordinate {
    uint8_t  l6_idx;
    uint8_t  l5_idx;
    uint8_t  l4_idx;
    uint16_t l3_idx;
    uint16_t l2_idx;
    uint8_t  l1_idx;
    uint16_t l0_idx;
} LinquCoordinate;

// =============================================================================
// LinquParam — identical semantics to PTOParam
// =============================================================================
typedef enum {
    LINQU_PARAM_INPUT  = 0,
    LINQU_PARAM_OUTPUT = 1,
    LINQU_PARAM_INOUT  = 2,
    LINQU_PARAM_SCALAR = 3,
} LinquParamType;

typedef struct LinquParam {
    LinquParamType type;
    uint64_t handle;         // tensor handle (INPUT/OUTPUT/INOUT)
    uint64_t scalar_value;   // raw value (SCALAR)
} LinquParam;

static inline LinquParam linqu_make_input(uint64_t h)  {
    LinquParam p = {LINQU_PARAM_INPUT, h, 0}; return p;
}
static inline LinquParam linqu_make_output(uint64_t h) {
    LinquParam p = {LINQU_PARAM_OUTPUT, h, 0}; return p;
}
static inline LinquParam linqu_make_inout(uint64_t h)  {
    LinquParam p = {LINQU_PARAM_INOUT, h, 0}; return p;
}
static inline LinquParam linqu_make_scalar(uint64_t v) {
    LinquParam p = {LINQU_PARAM_SCALAR, 0, v}; return p;
}

// =============================================================================
// LinquPeerList — returned by query_peers
// =============================================================================
typedef struct LinquPeerList {
    LinquCoordinate* peers;
    int count;
} LinquPeerList;

// =============================================================================
// LinquRuntimeOps — Unified ops table for ALL levels
// =============================================================================
//
// This is the SINGLE ops table type used at every level from L3 to L6.
// Follows the same pattern as simpler's PTO2RuntimeOps: the orchestration .so
// has zero link dependencies; all calls go through this function-pointer table.
//
// The ops table is populated by LinquOrchestratorState (same logic at all levels).
// L0–L2 use simpler's own PTO2RuntimeOps (separate codebase, not linked here).
//
typedef struct LinquRuntimeOps {
    // --- Core operations (present at every level) ---

    // Submit a task to a target node. Maps to LinquOrchestratorState::submit_task.
    void (*submit_task)(LinquRuntime* rt,
                        LinquCoordinate target,
                        const char* kernel_so,
                        LinquParam* params, int num_params);

    // Scope management
    void (*scope_begin)(LinquRuntime* rt);
    void (*scope_end)(LinquRuntime* rt);

    // Tensor lifecycle
    uint64_t (*alloc_tensor)(LinquRuntime* rt,
                             LinquCoordinate target,
                             size_t size_bytes);
    void (*free_tensor)(LinquRuntime* rt, uint64_t handle);

    // Orchestration completion
    void (*orchestration_done)(LinquRuntime* rt);

    // --- Extended operations (available at all levels, may be no-op at L0–L2) ---

    // Register external data on a remote node
    uint64_t (*reg_data)(LinquRuntime* rt,
                         LinquCoordinate target,
                         const void* data, size_t size);

    // Query peers at a given hierarchy level
    LinquPeerList (*query_peers)(LinquRuntime* rt, uint8_t level);

    // Self identity
    LinquCoordinate (*self_coord)(LinquRuntime* rt);

    // Wait for all submitted tasks to complete
    void (*wait_all)(LinquRuntime* rt);

    // Profiling: dump ring buffer snapshot
    void (*dump_ring_snapshot)(LinquRuntime* rt, const char* label);

    // Logging
    void (*log_error)(LinquRuntime* rt, const char* fmt, ...);
    void (*log_warn)(LinquRuntime* rt, const char* fmt, ...);
    void (*log_info)(LinquRuntime* rt, const char* fmt, ...);
    void (*log_debug)(LinquRuntime* rt, const char* fmt, ...);

    // Get tensor pool (for make_tensor/make_tensor_external to work)
    void* (*get_tensor_pool)(LinquRuntime* rt);

} LinquRuntimeOps;

// =============================================================================
// LinquRuntime — Opaque runtime pointer
// =============================================================================
struct LinquRuntime {
    const LinquRuntimeOps* ops;   // first field (C struct layout guarantee)
};

// =============================================================================
// LinquOrchConfig — configuration returned by .so entry point
// =============================================================================
typedef struct LinquOrchConfig {
    uint8_t level;              // pl.Level value (0..6)
    int expected_arg_count;
} LinquOrchConfig;

// =============================================================================
// .so Export Contract (same for ALL levels)
// =============================================================================
//
// Every orchestration .so — whether L0, L3, or L6 — exports:
//
//   extern "C" {
//     __attribute__((visibility("default")))
//     LinquOrchConfig linqu_orch_config(uint64_t* args, int arg_count);
//
//     __attribute__((visibility("default")))
//     void linqu_orch_entry(LinquRuntime* rt, uint64_t* args, int arg_count);
//   }
//
// The runtime reads linqu_orch_config().level to determine which
// LinquOrchestratorState instance to use when populating the ops table.
//

// =============================================================================
// Inline Convenience Wrappers (call through ops table)
// =============================================================================

static inline void linqu_submit_task(LinquRuntime* rt, LinquCoordinate target,
                                     const char* kernel_so,
                                     LinquParam* params, int num_params) {
    rt->ops->submit_task(rt, target, kernel_so, params, num_params);
}
static inline void linqu_scope_begin(LinquRuntime* rt) { rt->ops->scope_begin(rt); }
static inline void linqu_scope_end(LinquRuntime* rt)   { rt->ops->scope_end(rt); }
static inline uint64_t linqu_alloc_tensor(LinquRuntime* rt, LinquCoordinate t, size_t s) {
    return rt->ops->alloc_tensor(rt, t, s);
}
static inline void linqu_free_tensor(LinquRuntime* rt, uint64_t h) {
    rt->ops->free_tensor(rt, h);
}
static inline uint64_t linqu_reg_data(LinquRuntime* rt, LinquCoordinate t,
                                       const void* d, size_t s) {
    return rt->ops->reg_data(rt, t, d, s);
}
static inline LinquPeerList linqu_query_peers(LinquRuntime* rt, uint8_t level) {
    return rt->ops->query_peers(rt, level);
}
static inline LinquCoordinate linqu_self_coord(LinquRuntime* rt) {
    return rt->ops->self_coord(rt);
}
static inline void linqu_wait_all(LinquRuntime* rt) { rt->ops->wait_all(rt); }
static inline void linqu_dump_ring_snapshot(LinquRuntime* rt, const char* label) {
    rt->ops->dump_ring_snapshot(rt, label);
}
static inline void linqu_orchestration_done(LinquRuntime* rt) {
    rt->ops->orchestration_done(rt);
}

// =============================================================================
// RAII Scope Guard and Macro
// =============================================================================

#ifdef __cplusplus
class LinquScopeGuard {
public:
    LinquScopeGuard(LinquRuntime* rt) : rt_(rt) { rt_->ops->scope_begin(rt_); }
    ~LinquScopeGuard() { rt_->ops->scope_end(rt_); }
private:
    LinquRuntime* rt_;
};

#define _LINQU_CAT_IMPL(x, y) x ## y
#define _LINQU_CAT(x, y) _LINQU_CAT_IMPL(x, y)
#define LINQU_SCOPE_GUARD(rt) [[maybe_unused]] LinquScopeGuard _LINQU_CAT(lq_sg_, __COUNTER__)(rt)
#define LINQU_SCOPE(rt) if (LINQU_SCOPE_GUARD(rt); true)
#endif

// =============================================================================
// Logging Macros
// =============================================================================

#define LINQU_LOG_ERROR(rt, fmt, ...) (rt)->ops->log_error(rt, fmt, ##__VA_ARGS__)
#define LINQU_LOG_WARN(rt, fmt, ...)  (rt)->ops->log_warn(rt, fmt, ##__VA_ARGS__)
#define LINQU_LOG_INFO(rt, fmt, ...)  (rt)->ops->log_info(rt, fmt, ##__VA_ARGS__)
#define LINQU_LOG_DEBUG(rt, fmt, ...) (rt)->ops->log_debug(rt, fmt, ##__VA_ARGS__)

#endif // LINQU_ORCHESTRATION_API_H
```

### `LinquOrchestratorState` — The Unified Runtime Implementation

This is the **single C++ class** that implements the runtime for all levels (L3–L6):

```cpp
// src/runtime/linqu_orchestrator_state.h
// This is the RUNTIME-SIDE implementation (not seen by orchestration .so files)

struct LinquOrchestratorState {
    // === LEVEL IDENTITY (parameterizes all behavior) ===
    Level level;                    // Which level this instance serves
    LinquCoordinate coord;          // This node's coordinate

    // === RING BUFFERS (LinquHeapRing / LinquTaskRing) ===
    // Per-depth arrays, same algorithm at every level L3–L6
    LinquHeapRing    buffer_ring[MAX_SCOPE_DEPTH];
    LinquTaskRing    task_ring[MAX_SCOPE_DEPTH];
    LinquDepListPool dep_pool;

    // === TENSOR MAP (LinquTensorMap — handle-based exact match) ===
    // Local: hash table mapping tensor_handle → producer task_id
    // Extended: cross-node lookup for handles on remote nodes
    LinquTensorMap tensor_map;

    // === SCOPE STACK (LinquScopeStack) ===
    int32_t* scope_tasks;
    int32_t  scope_tasks_size;
    int32_t  scope_tasks_capacity;
    int32_t* scope_begins;
    int32_t  scope_stack_top;
    uint64_t scope_stack_capacity;

    // === DISPATCHER (level-dependent routing) ===
    // L3:    local execution or stub to L2 (future: ChipBackend adapter)
    // L4–L6: remote dispatch via IPC/network to child nodes
    LinquDispatcher* dispatcher;

    // === TRANSPORT (three-tier, level-dependent) ===
    // Tier 1 (L0–L2): shared device GM with atomics (via simpler, not managed here)
    // Tier 2 (L3↔L2): h2d_copy / d2h_copy DMA (managed by ChipBackend adapter)
    // Tier 3 (L3–L6): Unix sockets (verification) or TCP/RDMA (production)
    LinquTransport* transport;

    // === PEER REGISTRY ===
    LinquPeerRegistry* peers;

    // === STATISTICS ===
    int64_t tasks_submitted;
    int64_t buffers_allocated;
    int64_t bytes_allocated;

    // === PROFILING ===
    LinquOrchProfilingData profiling;
};
```

Key insight: `LinquHeapRing`, `LinquTaskRing`, `LinquDepListPool`, `LinquTensorMap` use the **same core algorithms** as `simpler`'s equivalents (O(1) bump allocation, ring wrap-around, lazy invalidation, chain truncation) but are **independent re-implementations** in `pypto_runtime_distributed` — no code is shared or linked. The key differences are the backing store and synchronization model:

| Component | L0–L2 (simpler, Tier 1) | L3–L6 (Linqu, Tier 2/3) |
|-----------|-------------------------|--------------------------|
| HeapRing backing | Device GM (shared memory, atomics) | `malloc`'d host memory (local, mutex) |
| TaskRing backing | Device GM task descriptors (LOAD_ACQUIRE/STORE_RELEASE) | Local `std::vector<LinquTaskDescriptor>` (mutex) |
| DepListPool backing | Device GM (shared) | Local allocation (single-process) |
| TensorMap key | GM buffer address (overlap detection) | Opaque integer handle (exact match) |
| Scheduler | Device-side worker queues (atomics) | Message-driven: TASK_COMPLETE → on_task_complete |
| L3↔L2 boundary | N/A (same address space) | `h2d_copy` / `d2h_copy` DMA via ChipBackend |

### How the Runtime Populates the Ops Table

When the daemon loads a `.so` and calls `linqu_orch_config()`, it gets the level tag. It then:

1. Creates (or reuses) a `LinquOrchestratorState` for that level.
2. Populates a `LinquRuntimeOps` struct with function pointers that call into the orchestrator state.
3. Wraps it in a `LinquRuntime` and calls `linqu_orch_entry(rt, args, arg_count)`.

The ops table delegates to `LinquOrchestratorState` at all levels (L3–L6):

```cpp
static LinquRuntimeOps make_linqu_ops() {
    return LinquRuntimeOps{
        .submit_task = [](LinquRuntime* rt, LinquCoordinate target,
                          const char* kernel_so, LinquParam* params, int n) {
            auto* state = get_state(rt);
            linqu_orch_submit_task(state, target, kernel_so, params, n);
        },
        .scope_begin = [](LinquRuntime* rt) {
            linqu_orch_scope_begin(get_state(rt));
        },
        .scope_end = [](LinquRuntime* rt) {
            linqu_orch_scope_end(get_state(rt));
        },
        // ... etc ...
    };
}
```

The implementation bodies (`linqu_orch_submit_task`, `linqu_orch_scope_begin`, etc.) are **the same code** regardless of whether `state->level` is HOST, POD, CLOS1, or CLOS2. The level only affects:

1. **Which ring capacity** is used (configured at init time).
2. **How the dispatcher routes tasks**: local execution (L3) vs remote IPC (L4–L6).
3. **Whether tensor map does cross-node lookup** (L4–L6) or only local lookup (L3).

### L0–L2 Boundary: Future `ChipBackend` Adapter (Not Part of Phase 0)

L0–L2 execution is handled by `simpler`, a separate codebase (`pypto_workspace/simpler/`). In Phase 0, `pypto_runtime_distributed` does **NOT** link against or call into `simpler`. The L3 daemon's downward dispatch to L2 is **stubbed** — it simulates chip-level completion without actual device execution.

In a future phase, a `ChipBackend` adapter will be implemented **within `pypto_runtime_distributed`** that:
- Calls `simpler`'s `PTO2OrchestratorState` through a stable ABI (dynamic linking to `libhost_runtime.so`).
- Manages `h2d_copy`/`d2h_copy` for host↔device data transfer.
- Maps Linqu's opaque tensor handles to device GM addresses.

This adapter does NOT modify `simpler` — it calls `simpler`'s existing public API.

### Relationship Between simpler Components and Linqu Re-implementations (Reference)

The table below shows the correspondence between `simpler` and Linqu components. These are **independent re-implementations** — no code is shared or linked between the two projects.

| simpler Component (L0–L2, reference) | Linqu Equivalent (L3–L6, implemented) | Difference |
|---------------------------------------|---------------------------------------|------------|
| `PTO2HeapRing` | `LinquHeapRing` | Same algorithm, host memory backing (not device GM) |
| `PTO2TaskRing` | `LinquTaskRing` | Same algorithm, extended descriptor (coordinate, level, fanin/fanout) |
| `PTO2DepListPool` | `LinquDepListPool` | Same algorithm |
| `PTO2TensorMap` (address-based overlap) | `LinquTensorMap` (handle-based exact match) | No overlap detection (unnecessary without shared buffer space) |
| `PTO2OrchestratorState` | `LinquOrchestratorState` | Same orchestration logic, mutex instead of atomics |
| `PTO2SchedulerState` (device-side, atomics) | Message-driven scheduler in `LinquOrchestratorState` | TASK_COMPLETE messages instead of shared-memory polling |
| `PTO2RuntimeOps` | `LinquRuntimeOps` | Extended with `query_peers`, `self_coord`, `reg_data`, `dump_ring_snapshot` |

### PyPTO Compiler Output Convention

All test programs are implemented as **hand-written C++** that looks exactly like PyPTO compiler output. Every `.so` file — at any level — exports the same two functions:

```cpp
extern "C" {
    __attribute__((visibility("default")))
    LinquOrchConfig linqu_orch_config(uint64_t* args, int arg_count);

    __attribute__((visibility("default")))
    void linqu_orch_entry(LinquRuntime* rt, uint64_t* args, int arg_count);
}
```

Inside `linqu_orch_entry`, the function uses ops table calls that work **identically at every level**:

| Operation | What it does |
|-----------|-------------|
| `linqu_submit_task(rt, target, "kernel.so", params, n)` | Allocate task slot from `task_ring`, lookup inputs in `tensor_map`, register outputs, add to `scope_tasks` |
| `LINQU_SCOPE(rt) { ... }` | Push scope on scope stack; on exit, iterate scope's tasks, apply retirement tokens |
| `linqu_alloc_tensor(rt, target, size)` | Allocate from `buffer_ring` at current scope depth |
| `linqu_free_tensor(rt, handle)` | Set `task_freed = true`, apply early release token (pl.free semantics) |
| `linqu_wait_all(rt)` | Block until all submitted tasks complete |
| `linqu_query_peers(rt, level)` | Query peer registry for nodes at given level |
| `linqu_self_coord(rt)` | Return this node's coordinate |

### How All Test Programs Use This Convention

| Test Program | Level | Key Ops Used |
|-------------|-------|-------------|
| **Topology:** `topo_L6_cluster.so` | L6 | `query_peers(5)`, `submit_task` to L5 supernodes, `LINQU_SCOPE`, `wait_all` |
| **Topology:** `topo_L5_supernode.so` | L5 | `query_peers(4)`, `submit_task` to L4 pods |
| **Topology:** `topo_L4_pod.so` | L4 | `query_peers(3)`, `submit_task` to L3 hosts |
| **Topology:** `topo_L3_host.so` | L3 | `self_coord`, `query_peers`, `log_info` |
| **DAG:** `kernel_add.so` etc. | L0 | Stubbed L0 compute (future: dispatched via ChipBackend to simpler) |
| **DAG:** `dag_L4_pod.so` | L4 | `submit_task`, `alloc_tensor`, `LINQU_SCOPE`, `wait_all` |
| **Ring:** `ring_phase1_L3_host.so` | L3 | `alloc_tensor`, `submit_task`, `LINQU_SCOPE`, `dump_ring_snapshot` |
| **Ring:** `ring_phase2_L3_host.so` | L3 | `alloc_tensor`, `free_tensor`, `LINQU_SCOPE` |
| **Ring:** `ring_phase4_L6_cluster.so` | L6 | Nested `submit_task` at L5→L4→L3→L0 |

**Deliverable:**
- `src/runtime/linqu_orchestration_api.h` — the unified ops table header (shown above)
- `src/runtime/linqu_orchestrator_state.h/.cpp` — the unified runtime implementation
- `docs/compiler_output_convention.md` — standalone reference

**Acceptance:** An engineer can write a new test `.so` for any level L3–L6 by including only `linqu_orchestration_api.h` and calling the same ops table functions. The runtime transparently routes to the correct backend. No dependency on `simpler` headers or libraries.

---

## Milestone 2: Core Identity Model (`src/core/`)

**Goal:** Implement the foundational data types that every other module depends on.

### Step 2.1: `pl::Level` Enum

**Action:** Implement the hierarchy level enum with all 7 levels and readability aliases.

```cpp
enum class Level : uint8_t {
    AIV = 0, AIC = 0, CORE_GROUP = 0,
    CHIP_DIE = 1, L2CACHE = 1,
    CHIP = 2, PROCESSOR = 2, UMA = 2,
    HOST = 3, NODE = 3,
    CLUSTER_0 = 4, POD = 4,
    CLUSTER_1 = 5, CLOS1 = 5,
    CLUSTER_2 = 6, CLOS2 = 6,
};
```

**Deliverable:** `src/core/level.h`, with unit tests verifying aliases resolve to correct integer values.

**Acceptance:** All 17 alias names compile and compare equal to their primary level.

### Step 2.2: `LinquCoordinate` Struct

**Action:** Implement the hierarchical coordinate struct:

```cpp
struct LinquCoordinate {
    uint8_t  l6_idx = 0;   // Cluster-level-2
    uint8_t  l5_idx = 0;   // Cluster-level-1
    uint8_t  l4_idx = 0;   // Cluster-level-0
    uint16_t l3_idx = 0;   // Host
    uint16_t l2_idx = 0;   // Chip
    uint8_t  l1_idx = 0;   // Chip die
    uint16_t l0_idx = 0;   // Core / core-group

    bool operator==(const LinquCoordinate&) const;
    std::string to_string() const;   // "L6.0/L5.1/L4.5/L3.20/L2.0/L1.0/L0.0"
    std::string to_path() const;     // "L6_0/L5_1/L4_5/L3_20" (for filesystem storage)
    void serialize(uint8_t* buf) const;
    static LinquCoordinate deserialize(const uint8_t* buf);
};
```

Note the `to_path()` method — this is used to create the per-node storage folders in the verification environment.

**Deliverable:** `src/core/coordinate.h`, unit tests for construction, comparison, serialization round-trip, `to_path()` output.

**Acceptance:** Serialize → deserialize → equality verified. `to_path()` of `{l6=0, l5=1, l4=5, l3=20}` → `"L6_0/L5_1/L4_5/L3_20"`.

### Step 2.3: `TaskKey` Struct

**Action:** Implement the full task identity:

```cpp
struct TaskKey {
    std::string  logical_system;
    LinquCoordinate coord;
    uint16_t     scope_depth;
    uint32_t     task_id;
};
```

Include equality, hash function, serialization, `to_string()`.

**Deliverable:** `src/core/task_key.h`, unit tests.

### Step 2.4: `NodeIdentity` Struct

**Action:** Implement the node identity model:

```cpp
struct NodeIdentity {
    std::string linqu_physical_system_name;
    std::string linqu_logical_system_name;
    LinquCoordinate coord;
    std::string ip_address;  // or "process://<pid>" for multi-process mode
};
```

**Deliverable:** `src/core/node_identity.h`, unit tests.

### Step 2.5: `get_my_coordinates()` Interface

**Action:** Define the user-pluggable coordinate mapping interface:

```cpp
using CoordinateMapper = std::function<LinquCoordinate(const std::string& address)>;
```

Provide three implementations:
1. **IP-based mapper:** `"10.B.C.D"` → `{l5=B, l4=C, l3=D}`.
2. **Single-host mapper:** returns all zeros except `l3_idx = 1`.
3. **Process-based mapper (for verification env):** reads coordinates from environment variables `LINQU_L6`, `LINQU_L5`, `LINQU_L4`, `LINQU_L3` set at process launch.

**Deliverable:** `src/core/coordinate_mapper.h`, unit tests for all three implementations.

---

## Milestone 3: Ring Buffers — Components of `LinquOrchestratorState` (`src/ring/`)

**Goal:** Implement `LinquHeapRing`, `LinquTaskRing`, and `LinquDepListPool` — the ring buffer subsystems that live inside `LinquOrchestratorState`. These use the **same core algorithms** as `simpler`'s equivalents but are **independent re-implementations** adapted to work with local host memory instead of device shared memory. No `simpler` code is included or linked.

### Step 3.1: `LinquHeapRing` — Buffer Allocation Ring

**Action:** Implement the heap ring buffer (same algorithm as `simpler`'s `PTO2HeapRing`, independent implementation):

```cpp
// O(1) bump allocation, wrap-around, back-pressure
// Backing store: malloc'd host memory (not device GM)
struct LinquHeapRing {
    void*    base;
    uint64_t size;
    uint64_t top;
    uint64_t tail;   // directly owned (no shared memory indirection)
};

void* linqu_heap_ring_alloc(LinquHeapRing* ring, uint64_t size);
void  linqu_heap_ring_retire_to(LinquHeapRing* ring, uint64_t new_tail);
void  linqu_heap_ring_reset(LinquHeapRing* ring);
```

**Deliverable:** `src/ring/linqu_heap_ring.h/.cpp`, unit tests.

**Acceptance:** Ring of 32KB: allocate 4×8KB → full → retire first 8KB → allocate 8KB succeeds with wrap-around.

### Step 3.2: `LinquTaskRing` — Task Slot Ring

**Action:** Implement the task ring (same algorithm as `simpler`'s `PTO2TaskRing`, independent implementation):

```cpp
struct LinquTaskDescriptor {
    TaskKey    key;
    uint32_t   fanout_count = 1;   // starts at 1 (scope-exit token)
    uint32_t   ref_count = 0;
    bool       task_freed = false;  // for pl.free optimization
    int32_t    dep_list_head = -1;  // offset into dep pool
    enum Status { PENDING, RUNNING, COMPLETED } status = PENDING;
    // + output buffer offset, kernel_so name, params
};

struct LinquTaskRing {
    LinquTaskDescriptor* descriptors;
    int32_t window_size;          // power of 2
    int32_t current_index;
    int32_t last_task_alive;      // directly owned (no shared memory)
};

int32_t linqu_task_ring_alloc(LinquTaskRing* ring);
void    linqu_task_ring_reset(LinquTaskRing* ring);
```

**Deliverable:** `src/ring/linqu_task_ring.h/.cpp`, unit tests.

### Step 3.3: `LinquDepListPool` — Dependency List Pool

**Action:** Implement the dep list pool (same algorithm as `simpler`'s `PTO2DepListPool`, independent implementation):

```cpp
struct LinquDepListPool {
    LinquDepListEntry* base;
    int32_t capacity;
    int32_t top;
};

int32_t linqu_dep_list_prepend(LinquDepListPool* pool, int32_t head, int32_t task_id);
```

**Deliverable:** `src/ring/linqu_dep_pool.h/.cpp`, unit tests.

### Step 3.4: Per-Depth Ring Arrays

**Action:** Each `LinquOrchestratorState` holds per-depth arrays of rings:

```cpp
static constexpr int MAX_SCOPE_DEPTH = 8;

// Inside LinquOrchestratorState:
LinquTaskRing    task_ring[MAX_SCOPE_DEPTH];
LinquHeapRing    buffer_ring[MAX_SCOPE_DEPTH];
```

**Deliverable:** Integration into `LinquOrchestratorState`, unit tests.

**Acceptance:** Create orchestrator state for Level=HOST, max_depth=4. Allocate tasks at depth 0 and 1. Retire depth 1 independently.

---

## Milestone 4: Scope Stack and TensorMap — Components of `LinquOrchestratorState`

**Goal:** Implement the scope stack and tensor map subsystems that live inside `LinquOrchestratorState`. These use the same core logic as `simpler`'s scope management and `PTO2TensorMap` but are independent re-implementations.

### Step 4.1: Scope Stack

**Action:** Implement the scope stack:

```cpp
// Inside LinquOrchestratorState:
int32_t* scope_tasks;          // flat buffer of task IDs (all scopes concatenated)
int32_t  scope_tasks_size;
int32_t  scope_tasks_capacity;
int32_t* scope_begins;         // scope_begins[i] = start index in scope_tasks
int32_t  scope_stack_top;      // -1 = no scope open
```

Functions:
- `linqu_scope_begin(state)` — push new scope: `scope_begins[++top] = scope_tasks_size`.
- `linqu_scope_end(state)` — pop scope: iterate `scope_tasks[scope_begins[top]..size)`, for each task apply scope-exit token.

**Deliverable:** `src/runtime/linqu_scope.h/.cpp`, unit tests.

### Step 4.2: Scope-Exit Token Logic

**Action:** On scope exit, for each task in the scope:
- If `task_freed == true` → skip (pl.free already applied).
- Else → `ref_count += 1` (scope token). If `ref_count == fanout_count` → task is reclaimable.

This is the standard scope-exit logic: iterate tasks, apply retirement tokens, trigger ring pointer advancement.

**Deliverable:** Part of `linqu_scope_end`, unit tests.

### Step 4.3: `pl.free()` — Early Release

**Action:** `linqu_free_tensor(state, handle)`:
- Find task that produced handle.
- If `task_freed == false`: set `task_freed = true`, apply token (`ref_count += 1`).
- If `task_freed == true`: no-op (idempotent).

**Deliverable:** Part of `LinquOrchestratorState`, unit tests.

### Step 4.4: `LinquTensorMap` — Local + Cross-Node Extension

**Action:** Implement the tensor map:

```cpp
// Hash-table + ring-buffer entry pool for handle→producer mapping
struct LinquTensorMap {
    int32_t*              buckets;
    int32_t               num_buckets;
    LinquTensorMapEntry*  entry_pool;
    int32_t               pool_size;
    int32_t               next_entry_idx;
    int32_t*              task_entry_head;  // per-task head offset
    int32_t               last_task_alive;

    // Extension for L4–L6: cross-node lookup
    LinquTransport*       transport;        // for remote queries (NULL at L3)
    LinquPeerRegistry*    peers;
    std::unordered_map<uint64_t, LinquTensorMapEntry> remote_cache;
};
```

**Functions:**
- `linqu_tensormap_insert(tm, handle, task_id, with_alloc)` — insert handle→producer mapping, with per-task chain linking and INOUT optimization.
- `linqu_tensormap_lookup(tm, handle, result)` — local lookup first; if miss AND `transport != NULL`, broadcast `TENSOR_LOOKUP` to peers.
- `linqu_tensormap_cleanup_retired(tm, old, new)` — invalidate entries for retired tasks.

**Deliverable:** `src/runtime/linqu_tensormap.h/.cpp`, unit tests.

---

## Milestone 5: Dispatcher — Level-Dependent Routing

**Goal:** Implement the level-dependent parts of `LinquOrchestratorState`: the dispatcher that routes tasks to the correct execution backend.

### Step 5.1: `LinquDispatcher` Interface

**Action:** Define the abstract dispatcher that `LinquOrchestratorState::submit_task` calls:

```cpp
class LinquDispatcher {
public:
    virtual void dispatch(LinquTaskDescriptor* task,
                          LinquCoordinate target,
                          const char* kernel_so,
                          LinquParam* params, int num_params) = 0;
    virtual void wait_all() = 0;
    virtual ~LinquDispatcher() = default;
};
```

**Deliverable:** `src/runtime/linqu_dispatcher.h`.

### Step 5.2: `LocalDispatcher` — L3 Host Backend

**Action:** Dispatches tasks locally on the same host:
- `dlopen()`s the kernel `.so`, resolves entry points, and executes synchronously or via a worker thread.
- For L3→L2 dispatch (chip-level tasks), uses a **stub** in Phase 0 that simulates task completion without actual device execution. The stub records the dispatch, simulates a configurable latency, and returns completion status.
- In a future phase, the `ChipBackend` adapter (see Step 5.5) replaces the stub for actual device dispatch.

**Deliverable:** `src/runtime/local_dispatcher.h/.cpp`.

### Step 5.3: `RemoteDispatcher` — L4–L6 Network Backend

**Action:** Dispatches tasks to child nodes via IPC/network:
- Serializes the task (kernel `.so` name, params, target coordinate).
- Sends via `LinquTransport` to the target daemon.
- The target daemon creates its own `LinquOrchestratorState` and runs the `.so`.

**Deliverable:** `src/runtime/remote_dispatcher.h/.cpp`.

### Step 5.4: `MockDispatcher` — Testing Backend

**Action:** Simulates task execution for unit testing:
- `dispatch()` → records the call, optionally sleeps, marks complete.
- Configurable latency and failure modes.

**Deliverable:** `src/runtime/mock_dispatcher.h`.

### Step 5.5: `ChipBackend` Adapter — L3→L2 Bridge (Future Phase, Design Only in Phase 0)

**Action (Phase 0):** Document the `ChipBackend` adapter interface — the Tier 2 bridge between Linqu's host memory (L3) and `simpler`'s device GM (L0–L2). No implementation in Phase 0; the `LocalDispatcher` uses a stub instead.

**Future implementation responsibilities (NOT in Phase 0):**
- Dynamic linking to `simpler`'s `libhost_runtime.so` (via `dlopen`, no compile-time dependency).
- `h2d_copy`: transfer input tensors from host memory to device GM before chip-level task submission.
- `d2h_copy`: transfer output tensors from device GM back to host memory after chip-level completion.
- Handle→device GM address mapping table.
- Device GM buffer lifecycle management (allocate on `h2d_copy`, release on scope retirement).

**Key constraint:** The adapter calls `simpler`'s existing public API through a stable ABI. It does NOT modify `simpler` source code. It lives entirely within `pypto_runtime_distributed`.

**Deliverable (Phase 0):** `docs/chip_backend_adapter_spec.md` — interface specification for the future adapter.

---

## Milestone 6: `LinquHeader` and Message Protocol (`src/transport/`)

**Goal:** Define the binary wire format for all inter-node messages, and implement the **IPC transport** for the multi-process verification environment.

### Step 6.1: `LinquHeader` Struct

**Action:** Implement the 24-byte message header with serialize/deserialize in network byte order.

**Deliverable:** `src/transport/linqu_header.h`, unit tests.

### Step 6.2: Message Type Enum and Payload Structs

**Action:** Define all 7 message types (`HEARTBEAT`, `REG_CODE`, `REG_DATA`, `CALL_TASK`, `SCOPE_EXIT`, `RETRY_WITH_CODE`, `TASK_COMPLETE`) with serializable payload structs.

Key payloads:

```cpp
struct CallTaskPayload {
    uint64_t blob_hash;          // hash of the .so file
    uint32_t spmd_idx;           // SPMD index for this node
    uint16_t scope_depth;
    uint32_t task_id;
    uint32_t num_data_handles;
    uint64_t data_handles[];     // variable-length array of handles
};

struct RegCodePayload {
    uint64_t blob_hash;
    uint32_t code_size;
    // followed by code_size bytes of .so binary
};

struct ScopeExitPayload {
    uint16_t scope_depth;
};

struct TaskCompletePayload {
    TaskKey  task_key;
    int32_t  status;  // 0=success, nonzero=error code
};
```

**Deliverable:** `src/transport/msg_types.h`, `src/transport/messages.h`.

### Step 6.3: IPC Transport for Multi-Process Verification

**Action:** Implement an IPC transport layer that allows hierarchy nodes (running as separate processes on the same machine) to communicate:

```cpp
class Transport {
public:
    virtual void send(const LinquCoordinate& target, const LinquHeader& hdr,
                      const uint8_t* payload, size_t len) = 0;
    virtual bool recv(LinquHeader& hdr, std::vector<uint8_t>& payload,
                      int timeout_ms = -1) = 0;
};
```

Implement `UnixSocketTransport`:
- Each process listens on a **Unix domain socket** at a well-known path derived from the coordinate: `/tmp/linqu/<logical_system>/L6_<x>/L5_<y>/L4_<z>/L3_<w>/daemon.sock`.
- Sending to a target coordinate constructs the socket path and connects.
- This is a drop-in replacement for TCP sockets in the real cluster; the interface is identical.

**Deliverable:** `src/transport/unix_socket_transport.h/.cpp`, unit tests (two processes communicating).

**Acceptance:** Process A sends a `CALL_TASK` message to process B via Unix socket. Process B receives and deserializes it correctly.

---

## Milestone 7: Discovery and `PeerRegistry` (`src/discovery/`)

**Goal:** Implement the peer registry and multi-process discovery.

### Step 7.1: `PeerRegistry` Data Structure

**Action:** Implement the thread-safe membership table.

**Deliverable:** `src/discovery/peer_registry.h`, unit tests.

### Step 7.2: Multi-Process Discovery

**Action:** In the verification environment, discovery works by scanning the filesystem:

```cpp
class FilesystemDiscovery {
    std::string base_path;  // /tmp/linqu/<logical_system>/
    // Scans for daemon.sock files under base_path
    // Populates PeerRegistry with found nodes
    void discover();
};
```

Each process creates its socket file on startup. Other processes discover it by scanning the directory tree.

**Deliverable:** `src/discovery/filesystem_discovery.h/.cpp`.

**Acceptance:** Launch 4 processes. Each discovers the other 3 via filesystem scan.

### Step 7.3: Forward-Compatibility Mock Tests

**Action:** Write tests with fake multi-level peers at L4, L5, L6.

**Deliverable:** `tests/forward_compat/test_peer_registry_multi_level.cpp`.

---

## Milestone 8: Node Daemon (`src/daemon/`)

**Goal:** Implement the per-process node daemon that receives and executes tasks.

### Step 8.1: `NodeDaemon` — Process Entry Point

**Action:** Implement the node daemon that runs as a standalone process:

```cpp
class NodeDaemon {
    NodeIdentity identity;
    Transport& transport;
    PeerRegistry registry;
    LinquDispatcher* dispatcher;  // LocalDispatcher for L3, RemoteDispatcher for L4–6
    CodeCache code_cache;         // blob_hash → .so file path
    DataCache data_cache;         // data_handle → buffer pointer

    // Storage path for this node's files
    std::string storage_path;     // e.g., /tmp/linqu/test_sys/L6_0/L5_0/L4_0/L3_1/

    void run();  // event loop: recv messages, dispatch
};
```

The daemon:
1. Reads `LINQU_L6`, `LINQU_L5`, `LINQU_L4`, `LINQU_L3` environment variables to determine its coordinate.
2. Creates its storage directory: `<base>/<coord.to_path()>/`.
3. Opens a Unix domain socket at `<storage_path>/daemon.sock`.
4. Enters event loop: receives messages, processes them.

**Deliverable:** `src/daemon/node_daemon.h/.cpp`.

### Step 8.2: `CodeCache` — .so Loading

**Action:** Implement the code cache that stores received `.so` binaries:

```cpp
class CodeCache {
    std::string storage_path;
    std::unordered_map<uint64_t, std::string> hash_to_path;

    // Receive REG_CODE: write .so to storage_path/<hash>.so
    void register_code(uint64_t blob_hash, const uint8_t* data, size_t len);

    // Lookup: returns path to .so, or empty if not found
    std::string lookup(uint64_t blob_hash);

    // Load and execute: dlopen, resolve entry, call
    void execute(uint64_t blob_hash, uint64_t* args, int arg_count,
                 LinquDispatcher* dispatcher);
};
```

The `.so` file is saved to the node's storage directory on disk, then loaded via `dlopen()`.

**Deliverable:** `src/daemon/code_cache.h/.cpp`.

**Acceptance:** Write a `.so` to cache → lookup succeeds → `dlopen` + `dlsym("aicpu_orchestration_entry")` succeeds.

### Step 8.3: `DataCache` — Buffer Management

**Action:** Implement the data cache for received tensors:

```cpp
class DataCache {
    std::unordered_map<uint64_t, std::vector<uint8_t>> handle_to_buffer;

    void register_data(uint64_t handle, const uint8_t* data, size_t len);
    void* lookup(uint64_t handle);
};
```

**Deliverable:** `src/daemon/data_cache.h/.cpp`.

### Step 8.4: Message Handler Loop

**Action:** Implement the daemon's message dispatch:

```cpp
void NodeDaemon::handle_message(const LinquHeader& hdr, const uint8_t* payload) {
    switch (hdr.msg_type) {
        case MsgType::REG_CODE:
            code_cache.register_code(parse_reg_code(payload));
            break;
        case MsgType::REG_DATA:
            data_cache.register_data(parse_reg_data(payload));
            break;
        case MsgType::CALL_TASK: {
            auto task = parse_call_task(payload);
            auto so_path = code_cache.lookup(task.blob_hash);
            if (so_path.empty()) {
                send_retry_with_code(hdr.sender_coord, task.blob_hash);
                break;
            }
            // Execute via dispatcher (LocalDispatcher for L3, RemoteDispatcher for L4–6)
            execute_task(task, so_path);
            send_task_complete(hdr.sender_coord, task.task_key);
            break;
        }
        case MsgType::SCOPE_EXIT:
            handle_scope_exit(parse_scope_exit(payload));
            break;
        // ...
    }
}
```

**Deliverable:** `src/daemon/node_daemon.cpp`, handling all 7 message types.

### Step 8.5: `linqu_daemon` Executable

**Action:** Create the main entry point for the daemon process:

```cpp
// src/daemon/main.cpp
int main(int argc, char** argv) {
    auto coord = read_coordinates_from_env();
    auto storage = create_storage_dir(coord);
    auto transport = UnixSocketTransport(storage + "/daemon.sock");
    auto dispatcher = LocalDispatcher();  // or MockDispatcher for testing
    NodeDaemon daemon(coord, transport, &dispatcher, storage);
    daemon.run();
}
```

**Deliverable:** `src/daemon/main.cpp`, builds to `linqu_daemon` executable for arm64.

**Acceptance:** `./linqu_daemon` starts, creates socket file, responds to `HEARTBEAT` messages.

---

## Milestone 9: Dispatch Engine (`src/dispatch/`)

**Goal:** Implement the `pl.at(level=...)` dispatch path, including SPMD fan-out to remote processes.

### Step 9.1: `Dispatcher` Interface

**Action:** Define the central dispatch router that works with both local and remote nodes.

**Deliverable:** `src/dispatch/dispatcher.h`.

### Step 9.2: Level 0–2 Dispatch (Stubbed in Phase 0)

**Action:** For `Level ∈ {0, 1, 2}`, the `LocalDispatcher` uses a **stub** that simulates chip-level task completion. In a future phase, the `ChipBackend` adapter will replace the stub for actual device dispatch via `simpler`'s ABI (without modifying `simpler`).

**Deliverable:** Add to `Dispatcher`, unit test with `MockDispatcher`.

### Step 9.3: Level 3 Dispatch (Host-Level)

**Action:** Implement host-level dispatch via `LocalDispatcher`: local kernel execution and stub dispatch to L2.

**Deliverable:** Add to `Dispatcher`, unit test.

### Step 9.4: Level 4+ Dispatch (Remote via Transport)

**Action:** Implement distributed dispatch:

1. Query `PeerRegistry` for live nodes at the target level.
2. For each target node:
   a. Send `REG_CODE` if the node doesn't have the blob (check `code_registered` cache).
   b. Send `REG_DATA` for any input data.
   c. Send `CALL_TASK` with `spmd_idx`, `scope_depth`, `task_id`.
3. Wait for `TASK_COMPLETE` from all targets (or handle `RETRY_WITH_CODE`).

In the verification environment, "remote" means another process via Unix socket.

**Deliverable:** Add to `Dispatcher`, integration test with 2 daemon processes.

### Step 9.5: `pl.at()` Context Manager

**Action:** Implement the RAII `AtScope` that combines scope enter + dispatch + scope exit.

**Deliverable:** `src/dispatch/at_scope.h`, unit test.

---

## Milestone 10: The Hierarchical Test Program — Identity Dump & Topology Verification

**Goal:** Create a minimal test program — implemented as **PyPTO kernels at different hierarchy levels** — that verifies the virtual cluster infrastructure works correctly. Each kernel is a hand-written C++ `.so` that represents the output of the PyPTO compiler for a `pl.at(level=...)` block. The kernels do NOT perform computation; they validate the process-per-node architecture, coordinate mapping, IPC transport, peer registry, and hierarchical discovery.

### Step 10.1: Design the Test Program as Multi-Level PyPTO Kernels

**Action:** The topology test is a **4-level PyPTO program** that the compiler would emit as 4 `.so` files, one per hierarchy level. We hand-write them to skip the compiler step.

**Conceptual PyPTO source (what the user would write):**

```python
@pl.function(level=pl.Level.CLOS2)
def topology_test():
    """L6 kernel: top-level, launches L5 checks on each supernode."""
    for sn in pl.peers(level=pl.Level.CLOS1):
        with pl.at(level=pl.Level.CLOS1):
            verify_supernode(sn)

@pl.function(level=pl.Level.CLOS1)
def verify_supernode(sn):
    """L5 kernel: launches L4 pod checks within this supernode."""
    for pod in pl.peers(level=pl.Level.POD):
        with pl.at(level=pl.Level.POD):
            verify_pod(pod)

@pl.function(level=pl.Level.POD)
def verify_pod(pod):
    """L4 kernel: launches L3 identity/discovery on each host in this pod."""
    for host in pl.peers(level=pl.Level.HOST):
        with pl.at(level=pl.Level.HOST):
            host_identity_and_discover(host)

@pl.function(level=pl.Level.HOST)
def host_identity_and_discover(host):
    """L3 kernel: dumps identity, performs peer discovery, writes results."""
    pl.dump_identity()
    pl.discover_peers()
```

**Compiler output (what we hand-write in C++):**

| `.so` file | Level | `linqu_orch_config().level` | Description |
|-----------|-------|-----------------------------|-------------|
| `topo_L6_cluster.so` | L6 (CLOS2) | 6 | Iterates over L5 supernodes, dispatches `topo_L5_supernode.so` to each |
| `topo_L5_supernode.so` | L5 (CLOS1) | 5 | Iterates over L4 pods, dispatches `topo_L4_pod.so` to each |
| `topo_L4_pod.so` | L4 (POD) | 4 | Iterates over L3 hosts, dispatches `topo_L3_host.so` to each |
| `topo_L3_host.so` | L3 (HOST) | 3 | Dumps identity, performs peer discovery, writes JSON |

This decomposition mirrors the PyPTO function hierarchy: each `.so` operates **only** at its designated level and uses `linqu_submit_task` to dispatch the next lower level's `.so`.

**Phase A — Identity Dump (by `topo_L3_host.so`):** Each host kernel writes its identity to:

```
<base>/L6_0/L5_<s>/L4_<p>/L3_<h>/logs/identity.json
```

Content:

```json
{
    "pid": 12345,
    "coordinate": {"l6": 0, "l5": 3, "l4": 2, "l3": 7},
    "global_index": 215,
    "simulated_ip": "10.3.2.7",
    "socket_path": "/tmp/linqu/linqu_vcluster_1024/L6_0/L5_3/L4_2/L3_7/daemon.sock",
    "physical_system": "arm64-dev-server",
    "logical_system": "linqu_vcluster_1024",
    "timestamp": "2026-03-13T18:44:00Z",
    "level_names": {
        "L6": "CLOS2",
        "L5": "CLOS1 supernode 3",
        "L4": "POD 2 in supernode 3",
        "L3": "Host 7 in pod 2"
    }
}
```

**Phase B — Topology Discovery (by `topo_L3_host.so`):** After identity dump, the same L3 kernel queries peers and writes:

```
<base>/L6_0/L5_<s>/L4_<p>/L3_<h>/logs/discovery.json
```

Content:

```json
{
    "self": {"l5": 3, "l4": 2, "l3": 7},
    "same_pod_peers": [
        {"l3": 0, "status": "live", "socket": "...daemon.sock"},
        {"l3": 1, "status": "live", "socket": "...daemon.sock"},
        ...
        {"l3": 15, "status": "live", "socket": "...daemon.sock"}
    ],
    "same_pod_count": 16,
    "same_supernode_peers_by_pod": {
        "L4_0": 16,
        "L4_1": 16,
        "L4_2": 16,
        "L4_3": 16
    },
    "same_supernode_total": 64,
    "cluster_supernodes": 16,
    "cluster_total_hosts": 1024,
    "discovery_time_ms": 42
}
```

**Phase C — Hierarchical Verification:** The orchestrator collects all `identity.json` and `discovery.json` files and runs assertions:

1. Exactly 1024 `identity.json` files exist.
2. All `global_index` values are unique and cover `[0, 1023]`.
3. All coordinates are consistent: `global = l5*64 + l4*16 + l3`.
4. Every node discovered exactly 15 `same_pod_peers` (itself excluded).
5. Every node discovered exactly 64 `same_supernode_total`.
6. Every node discovered exactly 1024 `cluster_total_hosts`.
7. No `"status": "dead"` entries.
8. All socket paths exist on disk.

**Deliverable:** `examples/topology_test/README.md` with this design, `.so` source files in `examples/topology_test/kernels/`.

### Step 10.2: L3 Host Identity Kernel — `topo_L3_host.so`

**Action:** This is the L3-level PyPTO kernel. It uses the Linqu ops table to query its own identity and peers.

```cpp
// examples/topology_test/kernels/topo_L3_host.cpp
#include "linqu_orchestration_api.h"
#include <cstdio>

extern "C" {

__attribute__((visibility("default")))
LinquOrchConfig linqu_orch_config(uint64_t* args, int arg_count) {
    (void)args; (void)arg_count;
    return LinquOrchConfig{.level = 3, .expected_arg_count = 0};
}

__attribute__((visibility("default")))
void linqu_orch_entry(LinquRuntime* rt, uint64_t* args, int arg_count) {
    (void)args; (void)arg_count;

    LinquCoordinate self = linqu_self_coord(rt);
    int global = self.l5_idx * 64 + self.l4_idx * 16 + self.l3_idx;

    // Phase A: dump identity
    linqu_log_info(rt, "identity: l6=%d l5=%d l4=%d l3=%d global=%d",
        self.l6_idx, self.l5_idx, self.l4_idx, self.l3_idx, global);
    // Runtime writes identity.json to this node's logs/ directory

    // Phase B: discover peers at each level
    LinquPeerList pod_peers = linqu_query_peers(rt, 4);   // same pod
    LinquPeerList sn_peers  = linqu_query_peers(rt, 5);   // same supernode
    LinquPeerList all_peers = linqu_query_peers(rt, 6);   // cluster-wide

    linqu_log_info(rt, "discovery: pod=%d supernode=%d cluster=%d",
        pod_peers.count, sn_peers.count, all_peers.count);
    // Runtime writes discovery.json to this node's logs/ directory

    linqu_orchestration_done(rt);
}

}  // extern "C"
```

**Deliverable:** `examples/topology_test/kernels/topo_L3_host.cpp`, compiled to `topo_L3_host.so`.

**Acceptance:** After launching the cluster and dispatching this kernel to all hosts, `find /tmp/linqu/.../logs/identity.json | wc -l` returns 1024. Each file contains valid JSON with the correct coordinate.

### Step 10.3: L4 Pod Kernel — `topo_L4_pod.so`

**Action:** This L4-level kernel dispatches the L3 kernel to each host in the pod.

```cpp
// examples/topology_test/kernels/topo_L4_pod.cpp
#include "linqu_orchestration_api.h"

extern "C" {

__attribute__((visibility("default")))
LinquOrchConfig linqu_orch_config(uint64_t* args, int arg_count) {
    (void)args; (void)arg_count;
    return LinquOrchConfig{.level = 4, .expected_arg_count = 0};
}

__attribute__((visibility("default")))
void linqu_orch_entry(LinquRuntime* rt, uint64_t* args, int arg_count) {
    (void)args; (void)arg_count;

    LinquPeerList hosts = linqu_query_peers(rt, 3);  // L3 hosts in this pod

    LINQU_SCOPE(rt) {
        for (int i = 0; i < hosts.count; i++) {
            linqu_submit_task(rt, hosts.peers[i],
                "topo_L3_host.so", NULL, 0);
        }
        linqu_wait_all(rt);
    }

    linqu_orchestration_done(rt);
}

}  // extern "C"
```

**Deliverable:** `examples/topology_test/kernels/topo_L4_pod.cpp`.

### Step 10.4: L5 Supernode and L6 Cluster Kernels

**Action:** These upper-level kernels follow the same structural pattern.

```cpp
// examples/topology_test/kernels/topo_L5_supernode.cpp
extern "C" {
__attribute__((visibility("default")))
LinquOrchConfig linqu_orch_config(uint64_t* args, int arg_count) {
    (void)args; (void)arg_count;
    return LinquOrchConfig{.level = 5, .expected_arg_count = 0};
}

__attribute__((visibility("default")))
void linqu_orch_entry(LinquRuntime* rt, uint64_t* args, int arg_count) {
    (void)args; (void)arg_count;
    LinquPeerList pods = linqu_query_peers(rt, 4);  // L4 pods in this supernode
    LINQU_SCOPE(rt) {
        for (int i = 0; i < pods.count; i++)
            linqu_submit_task(rt, pods.peers[i], "topo_L4_pod.so", NULL, 0);
        linqu_wait_all(rt);
    }
    linqu_orchestration_done(rt);
}
}
```

```cpp
// examples/topology_test/kernels/topo_L6_cluster.cpp
extern "C" {
__attribute__((visibility("default")))
LinquOrchConfig linqu_orch_config(uint64_t* args, int arg_count) {
    (void)args; (void)arg_count;
    return LinquOrchConfig{.level = 6, .expected_arg_count = 0};
}

__attribute__((visibility("default")))
void linqu_orch_entry(LinquRuntime* rt, uint64_t* args, int arg_count) {
    (void)args; (void)arg_count;
    LinquPeerList supernodes = linqu_query_peers(rt, 5);  // L5 supernodes
    LINQU_SCOPE(rt) {
        for (int i = 0; i < supernodes.count; i++)
            linqu_submit_task(rt, supernodes.peers[i], "topo_L5_supernode.so", NULL, 0);
        linqu_wait_all(rt);
    }
    linqu_orchestration_done(rt);
}
}
```

**Execution flow:** The orchestrator loads `topo_L6_cluster.so` → it dispatches to 16 L5 supernodes → each dispatches to 4 L4 pods → each dispatches to 16 L3 hosts → 1024 identity dumps and discoveries.

**Deliverable:** `examples/topology_test/kernels/topo_L5_supernode.cpp`, `topo_L6_cluster.cpp`.

### Step 10.5: Peer Discovery (inside L3 kernel)

**Action:** The actual peer discovery logic is invoked by the L3 kernel through the ops table. The runtime handles the filesystem scanning and peer classification:

1. `linqu_query_peers(rt, 4)` → scans `daemon.sock` files sharing the same `(l6, l5, l4)` but different `l3`.
2. `linqu_query_peers(rt, 5)` → scans peers sharing the same `(l6, l5)` but different `l4`.
3. `linqu_query_peers(rt, 6)` → all peers.

The runtime checks socket accessibility (can connect and send `HEARTBEAT`) for each discovered peer.

**Deliverable:** Discovery logic in runtime (`src/discovery/filesystem_discovery.cpp`), invoked via `LinquRuntimeOps::query_peers`.

**Acceptance:** After discovery, every node's `discovery.json` reports 16 same-pod peers and 64 same-supernode peers.

### Step 10.4: Verification Script (Phase C)

**Action:** Write `examples/topology_test/verify_topology.sh` that collects all JSON files and runs the assertions:

```bash
#!/bin/bash
BASE="${LINQU_BASE:-/tmp/linqu/linqu_vcluster_1024}"

echo "=== Linqu Topology Verification ==="

# Check 1: Identity file count
ID_COUNT=$(find "$BASE" -name "identity.json" | wc -l)
echo "[CHECK 1] Identity files: $ID_COUNT (expected: 1024)"
[ "$ID_COUNT" -eq 1024 ] && echo "  PASS" || echo "  FAIL"

# Check 2: Global indices unique and cover [0, 1023]
INDICES=$(find "$BASE" -name "identity.json" -exec grep -o '"global_index": [0-9]*' {} \; \
    | awk -F': ' '{print $2}' | sort -n)
UNIQUE=$(echo "$INDICES" | sort -un | wc -l)
MIN=$(echo "$INDICES" | head -1)
MAX=$(echo "$INDICES" | tail -1)
echo "[CHECK 2] Unique indices: $UNIQUE, range: [$MIN, $MAX] (expected: 1024, [0, 1023])"
[ "$UNIQUE" -eq 1024 ] && [ "$MIN" -eq 0 ] && [ "$MAX" -eq 1023 ] \
    && echo "  PASS" || echo "  FAIL"

# Check 3: Coordinate consistency (global = l5*64 + l4*16 + l3)
# ... python one-liner or jq to parse and verify ...

# Check 4: Discovery - same_pod_count == 16 for all nodes
POD_OK=$(find "$BASE" -name "discovery.json" \
    -exec grep -l '"same_pod_count": 16' {} \; | wc -l)
echo "[CHECK 4] Nodes with 16 same-pod peers: $POD_OK (expected: 1024)"
[ "$POD_OK" -eq 1024 ] && echo "  PASS" || echo "  FAIL"

# Check 5: Discovery - same_supernode_total == 64 for all nodes
SN_OK=$(find "$BASE" -name "discovery.json" \
    -exec grep -l '"same_supernode_total": 64' {} \; | wc -l)
echo "[CHECK 5] Nodes with 64 same-supernode total: $SN_OK (expected: 1024)"
[ "$SN_OK" -eq 1024 ] && echo "  PASS" || echo "  FAIL"

# Check 6: Discovery - cluster_total_hosts == 1024 for all nodes
ALL_OK=$(find "$BASE" -name "discovery.json" \
    -exec grep -l '"cluster_total_hosts": 1024' {} \; | wc -l)
echo "[CHECK 6] Nodes with 1024 cluster total: $ALL_OK (expected: 1024)"
[ "$ALL_OK" -eq 1024 ] && echo "  PASS" || echo "  FAIL"

# Check 7: No dead peers
DEAD=$(find "$BASE" -name "discovery.json" -exec grep -l '"dead"' {} \; | wc -l)
echo "[CHECK 7] Nodes reporting dead peers: $DEAD (expected: 0)"
[ "$DEAD" -eq 0 ] && echo "  PASS" || echo "  FAIL"

# Check 8: All socket paths exist
SOCK_COUNT=$(find "$BASE" -name "daemon.sock" | wc -l)
echo "[CHECK 8] Socket files on disk: $SOCK_COUNT (expected: 1024)"
[ "$SOCK_COUNT" -eq 1024 ] && echo "  PASS" || echo "  FAIL"

echo ""
echo "=== Verification Complete ==="
```

**Deliverable:** `examples/topology_test/verify_topology.sh`.

**Acceptance:** All 8 checks pass on the running 1024-node cluster.

### Step 10.5: Full Test Flow

**Action:** Document the complete test procedure:

```bash
# 1. Create directory structure
./venv/create_topology.sh --verify

# 2. Launch 1024 daemon processes
./venv/launch_cluster.sh

# 3. Wait for initialization, check cluster status
./venv/cluster_status.sh

# 4. Trigger discovery on all daemons
./build/linqu_orchestrator --command discover

# 5. Verify all identity and discovery results
./examples/topology_test/verify_topology.sh

# 6. Shut down
./venv/shutdown_cluster.sh
```

**Deliverable:** `examples/topology_test/run_test.sh` combining all steps.

**Acceptance:** Full script runs on the arm64 machine and all 8 verification checks pass.

### Step 10.6: Sample Identity Output

For reference, here is what the identity and discovery files look like for node (l5=3, l4=2, l3=7):

**`L6_0/L5_3/L4_2/L3_7/logs/identity.json`:**
```json
{
    "pid": 54321,
    "coordinate": {"l6": 0, "l5": 3, "l4": 2, "l3": 7},
    "global_index": 215,
    "simulated_ip": "10.3.2.7",
    "socket_path": "/tmp/linqu/linqu_vcluster_1024/L6_0/L5_3/L4_2/L3_7/daemon.sock",
    "physical_system": "arm64-dev-server",
    "logical_system": "linqu_vcluster_1024",
    "timestamp": "2026-03-13T18:44:12Z"
}
```

**`L6_0/L5_3/L4_2/L3_7/logs/discovery.json`:**
```json
{
    "self": {"l5": 3, "l4": 2, "l3": 7},
    "same_pod_peers": [
        {"l3": 0, "status": "live"},
        {"l3": 1, "status": "live"},
        {"l3": 2, "status": "live"},
        {"l3": 3, "status": "live"},
        {"l3": 4, "status": "live"},
        {"l3": 5, "status": "live"},
        {"l3": 6, "status": "live"},
        {"l3": 8, "status": "live"},
        {"l3": 9, "status": "live"},
        {"l3": 10, "status": "live"},
        {"l3": 11, "status": "live"},
        {"l3": 12, "status": "live"},
        {"l3": 13, "status": "live"},
        {"l3": 14, "status": "live"},
        {"l3": 15, "status": "live"}
    ],
    "same_pod_count": 16,
    "same_supernode_peers_by_pod": {
        "L4_0": 16,
        "L4_1": 16,
        "L4_2": 16,
        "L4_3": 16
    },
    "same_supernode_total": 64,
    "cluster_supernodes": 16,
    "cluster_total_hosts": 1024,
    "discovery_time_ms": 85
}
```

---

## Milestone 10B: Distributed Tensor DAG Test — Cross-Node Dependency Verification

**Goal:** Build a test program — implemented as **PyPTO kernels at L0, L3, and L4** — that performs **actual tensor computation** across the cluster with **data dependencies that span hierarchy levels**. This validates that the distributed TensorMap can correctly track producer/consumer relationships across nodes, that scope-based lifetime management works at cluster scale, and that results are numerically correct.

The computation is modeled after `simpler`'s DAG example (`(a+b+1)(a+b+2)+(a+b)`) but lifted to cluster level, so that:
- Different tasks run on **different nodes** (cross-node data dependency).
- Intermediate tensors have **scoped lifetimes** (inner scope intermediates are freed at scope exit).
- The TensorMap tracks **who produces what** across the hierarchy.
- Final results are gathered and verified against a reference computed locally.

**Conceptual PyPTO source (what the user would write):**

```python
@pl.function(level=pl.Level.POD)
def distributed_dag(a, b):
    """L4 kernel: orchestrate the (a+b+1)(a+b+2)+(a+b) DAG across 4 hosts."""
    c = pl.empty_like(a)
    with pl.at(level=pl.Level.HOST, target=0):
        c = a + b                          # t0 on Host 0 (L0 kernel: kernel_add)

    with pl.at(level=pl.Level.HOST):       # inner scope
        with pl.at(target=1):
            d = c + 1.0                    # t1 on Host 1 (L0 kernel: kernel_add_scalar)
        with pl.at(target=2):
            e = c + 2.0                    # t2 on Host 2 (L0 kernel: kernel_add_scalar)
        with pl.at(target=3):
            g = d * e                      # t3 on Host 3 (L0 kernel: kernel_mul)
        with pl.at(target=0):
            f = g + c                      # t4 on Host 0 (L0 kernel: kernel_add)
    return f

# The L0 kernels (add, add_scalar, mul) use pl.at(level=pl.Level.CORE):
@pl.function(level=pl.Level.CORE)
def kernel_add(a, b, c, n):
    for i in range(n): c[i] = a[i] + b[i]
```

**Compiler output:** The PyPTO compiler would emit these `.so` files:

| `.so` file | Level | API | Description |
|-----------|-------|-----|-------------|
| `kernel_add.so` | L0 (CORE) | Pure C function | `c[i] = a[i] + b[i]` |
| `kernel_add_scalar.so` | L0 (CORE) | Pure C function | `c[i] = a[i] + scalar` |
| `kernel_mul.so` | L0 (CORE) | Pure C function | `c[i] = a[i] * b[i]` |
| `dag_L4_pod.so` | L4 (POD) | Linqu API | Orchestrates the DAG across 4 hosts |

The L0 kernels are dispatched by the Linqu runtime to `simpler` for execution; the L4 kernel uses the Linqu ops table.

### Step 10B.1: DAG Design

**Action:** Design a distributed DAG that computes `f = (a+b+1)(a+b+2) + (a+b)` across 4 nodes in a single pod.

```
Cluster DAG (4 hosts in one pod, L4_0):
═══════════════════════════════════════

Inputs:  a[1024], b[1024] — provided by orchestrator

Host L3_0 (outer scope):
  t0: c = a + b                     → produces tensor c
      c is registered in distributed TensorMap as produced by (L3=0, task=t0)

Host L3_1 (inner scope):
  t1: d = c + 1.0                   → consumes c from L3_0, produces d
      TensorMap lookup for c → finds producer (L3=0, t0) → data transfer L3_0 → L3_1
      d registered as produced by (L3=1, task=t1)

Host L3_2 (inner scope):
  t2: e = c + 2.0                   → consumes c from L3_0, produces e
      TensorMap lookup for c → finds producer (L3=0, t0) → data transfer L3_0 → L3_2
      e registered as produced by (L3=2, task=t2)

Host L3_3 (inner scope):
  t3: g = d * e                     → consumes d from L3_1 AND e from L3_2, produces g
      TensorMap lookup for d → producer (L3=1, t1) → transfer L3_1 → L3_3
      TensorMap lookup for e → producer (L3=2, t2) → transfer L3_2 → L3_3
      g registered as produced by (L3=3, task=t3)

Host L3_0 (inner scope, reuse):
  t4: f = g + c                     → consumes g from L3_3 AND c from self (L3_0), produces f
      TensorMap lookup for g → producer (L3=3, t3) → transfer L3_3 → L3_0
      TensorMap lookup for c → producer (L3=0, t0) → local, no transfer
      f is the final output.

Dependency graph:
  t0 ──→ t1 ──→ t3 ──→ t4
  t0 ──→ t2 ──┘       ↑
  t0 ──────────────────┘

Scope structure:
  pod scope {
      t0 on L3_0 [outer]         ← c lives until pod scope exit
      inner scope {
          t1 on L3_1             ← d freed at inner scope exit
          t2 on L3_2             ← e freed at inner scope exit
          t3 on L3_3             ← g freed at inner scope exit
          t4 on L3_0             ← f persists (output)
      }  // inner scope exit: d, e, g retired
  }  // pod scope exit: c retired

Expected output: f[i] = (a[i]+b[i]+1)(a[i]+b[i]+2) + (a[i]+b[i])
With a[i]=i, b[i]=i*2: f[i] = (3i+1)(3i+2) + 3i = 9i² + 12i + 2
```

**Key verification points:**
1. **Cross-node data dependency:** t1 on L3_1 consumes data produced by t0 on L3_0 → requires data transfer.
2. **Multi-producer fan-in:** t3 on L3_3 depends on outputs from both L3_1 and L3_2.
3. **TensorMap correctness:** Each node's TensorMap correctly records producers and discovers dependencies via the distributed lookup.
4. **Scope lifetime:** Inner scope intermediates (d, e, g) are freed at inner scope exit. c persists across the inner scope. f is the output.
5. **Numerical correctness:** f[i] verified against closed-form formula.

**Deliverable:** `examples/tensor_dag_test/README.md`.

### Step 10B.2: Distributed TensorMap

**Action:** Extend the TensorMap concept to cluster level. The distributed TensorMap is a **per-node local map + cross-node lookup protocol**:

```cpp
struct DistributedTensorEntry {
    uint64_t tensor_handle;        // hash of (base_ptr, offset, size)
    LinquCoordinate producer_coord;  // which node produced it
    uint32_t producer_task_id;
    uint16_t producer_scope_depth;
    uint64_t data_size_bytes;
};

class DistributedTensorMap {
    // Local map: tensors produced on this node
    std::unordered_map<uint64_t, DistributedTensorEntry> local_entries_;

    // Cache of remote lookups (avoid repeated network queries)
    std::unordered_map<uint64_t, DistributedTensorEntry> remote_cache_;

    // Register a locally produced tensor
    void insert(uint64_t handle, uint32_t task_id, uint16_t scope_depth, uint64_t size);

    // Lookup producer — first check local, then query remote nodes
    DistributedTensorEntry lookup(uint64_t handle);

    // Invalidate entries for retired tasks (scope exit)
    void retire_scope(uint16_t scope_depth);
};
```

When a node needs tensor data produced elsewhere:
1. **Lookup:** Query local TensorMap. If miss, broadcast `TENSOR_LOOKUP` to peers in same pod.
2. **Transfer:** The producer node responds with `TENSOR_DATA` containing the actual bytes.
3. **Cache:** The consumer caches the entry to avoid repeated lookups.

New message types for this:

```
TENSOR_LOOKUP  = 0x08   // Consumer → Pod peers: "who has tensor handle X?"
TENSOR_DATA    = 0x09   // Producer → Consumer: "here is the data for handle X"
```

**Deliverable:** `src/discovery/distributed_tensormap.h/.cpp`.

### Step 10B.3: L0 Compute Kernels (PyPTO kernel at `pl.Level.CORE`)

**Action:** Write 3 L0-level PyPTO kernels — pure C functions that would be emitted by the PyPTO compiler for `@pl.function(level=pl.Level.CORE)` blocks. These are compiled to `.so` and dispatched to `simpler` for execution on the actual hardware cores:

```cpp
// examples/tensor_dag_test/kernels/kernel_add.cpp
// C[i] = A[i] + B[i]
extern "C" void kernel_add(float* a, float* b, float* c, int n) {
    for (int i = 0; i < n; i++) c[i] = a[i] + b[i];
}

// examples/tensor_dag_test/kernels/kernel_add_scalar.cpp
// C[i] = A[i] + scalar
extern "C" void kernel_add_scalar(float* a, float scalar, float* c, int n) {
    for (int i = 0; i < n; i++) c[i] = a[i] + scalar;
}

// examples/tensor_dag_test/kernels/kernel_mul.cpp
// C[i] = A[i] * B[i]
extern "C" void kernel_mul(float* a, float* b, float* c, int n) {
    for (int i = 0; i < n; i++) c[i] = a[i] * b[i];
}
```

These compile to `kernel_add.so`, `kernel_add_scalar.so`, `kernel_mul.so`.

**Deliverable:** `examples/tensor_dag_test/kernels/`.

**Acceptance:** Each `.so` loads via `dlopen`, resolves the entry symbol, and computes correctly on test data.

### Step 10B.4: L4 Pod Orchestration Kernel — `dag_L4_pod.so` (PyPTO kernel at `pl.Level.POD`)

**Action:** Write the L4-level PyPTO kernel — the pod-level orchestration function that the compiler would emit for the `@pl.function(level=pl.Level.POD)` block. It builds and dispatches the DAG across 4 hosts, using `linqu_orchestration_api.h` and the distributed TensorMap:

```cpp
// examples/tensor_dag_test/kernels/dag_L4_pod.cpp
// PyPTO compiler output for: @pl.function(level=pl.Level.POD)
#include "linqu_orchestration_api.h"

extern "C" {

__attribute__((visibility("default")))
LinquOrchConfig linqu_orch_config(uint64_t* args, int arg_count) {
    (void)args; (void)arg_count;
    return LinquOrchConfig{.level = 4, .expected_arg_count = 4};
}

__attribute__((visibility("default")))
void linqu_orch_entry(LinquRuntime* rt, uint64_t* args, int arg_count) {
    float* a_ptr = (float*)(uintptr_t)args[0];
    float* b_ptr = (float*)(uintptr_t)args[1];
    float* f_ptr = (float*)(uintptr_t)args[2];  // output
    int n = (int)args[3];

    auto hosts = linqu_query_peers(rt, LINQU_LEVEL_HOST);
    // hosts[0]=L3_0, hosts[1]=L3_1, hosts[2]=L3_2, hosts[3]=L3_3

    // Register input data on host L3_0
    uint64_t h_a = linqu_reg_data(rt, hosts[0], a_ptr, n * sizeof(float));
    uint64_t h_b = linqu_reg_data(rt, hosts[0], b_ptr, n * sizeof(float));

    LINQU_SCOPE(rt) {
        // t0: c = a + b  (on L3_0, outer scope)
        uint64_t h_c = linqu_alloc_tensor(rt, hosts[0], n * sizeof(float));
        linqu_call_task(rt, hosts[0], "kernel_add.so",
            /*inputs=*/{h_a, h_b}, /*outputs=*/{h_c},
            /*scalars=*/{(uint64_t)n}, /*task_tag=*/"t0_add");

        LINQU_SCOPE(rt) {
            // t1: d = c + 1.0  (on L3_1)
            //   TensorMap: c produced by (L3_0, t0) → auto-transfer L3_0→L3_1
            uint64_t h_d = linqu_alloc_tensor(rt, hosts[1], n * sizeof(float));
            linqu_call_task(rt, hosts[1], "kernel_add_scalar.so",
                /*inputs=*/{h_c}, /*outputs=*/{h_d},
                /*scalars=*/{float_to_u64(1.0f), (uint64_t)n},
                /*task_tag=*/"t1_add_scalar");

            // t2: e = c + 2.0  (on L3_2)
            //   TensorMap: c produced by (L3_0, t0) → auto-transfer L3_0→L3_2
            uint64_t h_e = linqu_alloc_tensor(rt, hosts[2], n * sizeof(float));
            linqu_call_task(rt, hosts[2], "kernel_add_scalar.so",
                /*inputs=*/{h_c}, /*outputs=*/{h_e},
                /*scalars=*/{float_to_u64(2.0f), (uint64_t)n},
                /*task_tag=*/"t2_add_scalar");

            // t3: g = d * e  (on L3_3)
            //   TensorMap: d from (L3_1, t1), e from (L3_2, t2)
            //   → auto-transfer L3_1→L3_3 AND L3_2→L3_3
            uint64_t h_g = linqu_alloc_tensor(rt, hosts[3], n * sizeof(float));
            linqu_call_task(rt, hosts[3], "kernel_mul.so",
                /*inputs=*/{h_d, h_e}, /*outputs=*/{h_g},
                /*scalars=*/{(uint64_t)n},
                /*task_tag=*/"t3_mul");

            // t4: f = g + c  (on L3_0)
            //   TensorMap: g from (L3_3, t3) → transfer L3_3→L3_0
            //              c from (L3_0, t0) → local, no transfer
            uint64_t h_f = linqu_reg_data_handle(rt, hosts[0], f_ptr, n * sizeof(float));
            linqu_call_task(rt, hosts[0], "kernel_add.so",
                /*inputs=*/{h_g, h_c}, /*outputs=*/{h_f},
                /*scalars=*/{(uint64_t)n},
                /*task_tag=*/"t4_add_final");
        }
        // inner scope exit: d (L3_1), e (L3_2), g (L3_3) retired
    }
    // outer scope exit: c (L3_0) retired

    linqu_wait_all(rt);
}

}
```

**Deliverable:** `examples/tensor_dag_test/L4_dag_orch.cpp` → `L4_dag_orch.so`.

### Step 10B.5: Execution Trace and Dependency Log

**Action:** Each daemon writes a per-task execution trace to `logs/task_trace.json`:

```json
{
    "node": {"l5": 0, "l4": 0, "l3": 1},
    "tasks": [
        {
            "task_tag": "t1_add_scalar",
            "kernel": "kernel_add_scalar.so",
            "inputs": [
                {
                    "handle": "0xA1B2C3",
                    "producer_node": {"l3": 0},
                    "producer_task": "t0_add",
                    "transfer": "remote",
                    "transfer_bytes": 4096,
                    "transfer_time_us": 120
                }
            ],
            "outputs": [
                {
                    "handle": "0xD4E5F6",
                    "size_bytes": 4096
                }
            ],
            "execution_time_us": 45,
            "scope_depth": 2
        }
    ],
    "scope_exits": [
        {
            "depth": 2,
            "retired_tensors": ["0xD4E5F6"],
            "retired_bytes": 4096
        }
    ]
}
```

This trace is the primary evidence that:
- The distributed TensorMap correctly resolved data dependencies.
- Cross-node data transfers happened when expected.
- Scope retirement freed the right tensors at the right time.

**Deliverable:** Trace logging in `src/daemon/node_daemon.cpp`.

### Step 10B.6: Verification Script

**Action:** Write `examples/tensor_dag_test/verify_dag.sh`:

```bash
#!/bin/bash
BASE="${LINQU_BASE}/L6_0/L5_0/L4_0"

echo "=== Distributed Tensor DAG Verification ==="

# Check 1: Numerical correctness
# f[i] = (a[i]+b[i]+1)(a[i]+b[i]+2) + (a[i]+b[i])
# With a[i]=i, b[i]=i*2: f[i] = (3i+1)(3i+2) + 3i = 9i²+12i+2
echo "[CHECK 1] Numerical correctness of output f[]"
./build/linqu_verify_result --input "$BASE/L3_0/data_cache/result.bin" \
    --formula "9*i*i + 12*i + 2" --count 1024
# prints PASS or FAIL with first mismatch

# Check 2: Cross-node transfers happened
echo "[CHECK 2] Cross-node data transfers"
L3_1_TRACE="$BASE/L3_1/logs/task_trace.json"
REMOTE_IN=$(grep -c '"transfer": "remote"' "$L3_1_TRACE")
echo "  L3_1 remote inputs: $REMOTE_IN (expected: ≥1, c from L3_0)"
[ "$REMOTE_IN" -ge 1 ] && echo "  PASS" || echo "  FAIL"

L3_3_TRACE="$BASE/L3_3/logs/task_trace.json"
REMOTE_IN_3=$(grep -c '"transfer": "remote"' "$L3_3_TRACE")
echo "  L3_3 remote inputs: $REMOTE_IN_3 (expected: ≥2, d from L3_1 + e from L3_2)"
[ "$REMOTE_IN_3" -ge 2 ] && echo "  PASS" || echo "  FAIL"

# Check 3: Dependency correctness — t3 depends on t1 AND t2
echo "[CHECK 3] Task t3 (mul) consumed outputs from t1 and t2"
T3_PRODUCERS=$(grep -A5 '"t3_mul"' "$L3_3_TRACE" | grep '"producer_task"' | sort -u)
echo "  Producers for t3: $T3_PRODUCERS"
echo "$T3_PRODUCERS" | grep -q "t1_add_scalar" && echo "  t1→t3: PASS" || echo "  t1→t3: FAIL"
echo "$T3_PRODUCERS" | grep -q "t2_add_scalar" && echo "  t2→t3: PASS" || echo "  t2→t3: FAIL"

# Check 4: Scope retirement — inner scope freed d, e, g
echo "[CHECK 4] Inner scope retirement"
for l3 in 1 2 3; do
    TRACE="$BASE/L3_${l3}/logs/task_trace.json"
    RETIRED=$(grep -c '"retired_tensors"' "$TRACE")
    echo "  L3_${l3} scope retirements: $RETIRED (expected: ≥1)"
    [ "$RETIRED" -ge 1 ] && echo "  PASS" || echo "  FAIL"
done

# Check 5: c (outer scope tensor) was NOT retired during inner scope
echo "[CHECK 5] Tensor c survived inner scope (used by t4 after inner scope tasks)"
L3_0_TRACE="$BASE/L3_0/logs/task_trace.json"
T4_INPUT=$(grep -A5 '"t4_add_final"' "$L3_0_TRACE" | grep '"transfer": "local"')
echo "  t4 local input (c): $([ -n "$T4_INPUT" ] && echo 'PASS' || echo 'FAIL')"

# Check 6: All tasks completed
echo "[CHECK 6] All 5 tasks completed across the pod"
TOTAL_TASKS=0
for l3 in 0 1 2 3; do
    TRACE="$BASE/L3_${l3}/logs/task_trace.json"
    COUNT=$(grep -c '"task_tag"' "$TRACE" 2>/dev/null || echo 0)
    TOTAL_TASKS=$((TOTAL_TASKS + COUNT))
done
echo "  Total tasks: $TOTAL_TASKS (expected: 5)"
[ "$TOTAL_TASKS" -eq 5 ] && echo "  PASS" || echo "  FAIL"

echo ""
echo "=== DAG Verification Complete ==="
```

**Deliverable:** `examples/tensor_dag_test/verify_dag.sh`.

### Step 10B.7: Full DAG Test Flow

**Action:** Write `examples/tensor_dag_test/run_test.sh`:

```bash
#!/bin/bash
# Run the distributed tensor DAG test on 4 hosts in one pod

# 1. Launch 4 daemons (one pod: l5=0, l4=0, l3=0..3)
for l3 in 0 1 2 3; do
    LINQU_SYSTEM=dag_test LINQU_L6=0 LINQU_L5=0 LINQU_L4=0 LINQU_L3=$l3 \
    LINQU_BASE=/tmp/linqu/dag_test LINQU_USE_MOCK_CHIP=1 \
    ./build/linqu_daemon &
done
sleep 2

# 2. Run the DAG orchestration
./build/linqu_orchestrator --command run_dag \
    --so examples/tensor_dag_test/build/L4_dag_orch.so \
    --a "iota" --b "iota_x2" --size 1024

# 3. Verify
./examples/tensor_dag_test/verify_dag.sh

# 4. Cleanup
./venv/shutdown_cluster.sh --base /tmp/linqu/dag_test
```

**Deliverable:** `examples/tensor_dag_test/run_test.sh`.

**Acceptance:** All 6 verification checks pass: numerical correctness, cross-node transfers, dependency tracking, scope retirement, tensor lifetime, task completion.

### Step 10B.8: Scale Up — Full Cluster DAG (L5 and L6 Kernels)

**Action:** Extend the DAG test to run across the full 1024-node cluster using additional PyPTO kernels at L5 and L6. The same 5-task DAG pattern is replicated independently in each pod (64 pods total), producing 64 × 5 = 320 tasks.

**Additional compiler-output `.so` files for cluster-scale execution:**

| `.so` file | Level | Description |
|-----------|-------|-------------|
| `dag_L5_supernode.so` | L5 (CLOS1) | Iterates over 4 pods in this supernode, dispatches `dag_L4_pod.so` to each |
| `dag_L6_cluster.so` | L6 (CLOS2) | Iterates over 16 supernodes, dispatches `dag_L5_supernode.so` to each |

```cpp
// examples/tensor_dag_test/kernels/dag_L5_supernode.cpp
// PyPTO compiler output for: @pl.function(level=pl.Level.CLOS1)
extern "C" {
__attribute__((visibility("default")))
LinquOrchConfig linqu_orch_config(uint64_t* args, int arg_count) {
    (void)args; (void)arg_count;
    return LinquOrchConfig{.level = 5, .expected_arg_count = 4};
}

__attribute__((visibility("default")))
void linqu_orch_entry(LinquRuntime* rt, uint64_t* args, int arg_count) {
    LinquPeerList pods = linqu_query_peers(rt, 4);
    int n_per_pod = (int)(args[3]) / pods.count;
    LINQU_SCOPE(rt) {
        for (int i = 0; i < pods.count; i++) {
            uint64_t pod_args[4] = {
                args[0] + i * n_per_pod * sizeof(float),  // a chunk offset
                args[1] + i * n_per_pod * sizeof(float),  // b chunk offset
                args[2] + i * n_per_pod * sizeof(float),  // f chunk offset
                (uint64_t)n_per_pod
            };
            linqu_submit_task(rt, pods.peers[i], "dag_L4_pod.so",
                (LinquParam*)pod_args, 4);
        }
        linqu_wait_all(rt);
    }
    linqu_orchestration_done(rt);
}
}
```

**Execution flow:** Orchestrator loads `dag_L6_cluster.so` → dispatches to 16 L5 supernodes → each dispatches to 4 L4 pods → each pod runs 5-task DAG across 4 L3 hosts → 320 total tasks.

This tests:
- **Hierarchical dispatch through the full L6→L5→L4→L3→L0 kernel stack.**
- **Independent pod execution:** 64 pods run in parallel.
- **Global correctness:** Final concatenated result matches reference.

**Deliverable:** `examples/tensor_dag_test/kernels/dag_L5_supernode.cpp`, `dag_L6_cluster.cpp`, `run_cluster_dag.sh`.

**Acceptance:** 64 pods × 5 tasks = 320 tasks complete. Output verified for all 1024 × 64 = 65536 elements.

---

## Milestone 10C: Multi-Level Ring Stress Test — Allocation, Reuse, and Retirement at Every Hierarchy

**Goal:** Build a dedicated test program — implemented as **PyPTO kernels at L0, L3, L4, L5, and L6** — that exercises `task_ring[L][d]` and `buffer_ring[L][d]` at **every hierarchy level** (L3–L6, with L0–L2 inside `simpler`). The test deliberately stresses ring capacity, forces ring wrap-around and slot reuse, validates scope-based retirement, tests `pl.free()` early release, and verifies the 6 correctness invariants from the design doc (§5.6). Every ring operation produces profiling metrics that are checked against expected values.

Unlike the DAG test (Milestone 10B) which focuses on data dependency correctness, this test focuses on **memory management correctness**: are ring slots allocated, retired, and reused correctly across levels and depths?

**Conceptual PyPTO source (what the user would write):**

```python
@pl.function(level=pl.Level.CLOS2)
def ring_stress_full(config):
    """L6 kernel: drives L5→L4→L3 nested ring allocation."""
    with pl.at(level=pl.Level.CLOS1):
        ring_stress_supernode(config)

@pl.function(level=pl.Level.CLOS1)
def ring_stress_supernode(config):
    """L5 kernel: allocates at CLOS1 ring, nests L4 calls."""
    t_sn = pl.alloc_tensor(config.sn_size)
    with pl.at(level=pl.Level.POD):
        ring_stress_pod(config)

@pl.function(level=pl.Level.POD)
def ring_stress_pod(config):
    """L4 kernel: allocates at POD ring, nests L3 calls."""
    for host in pl.peers(level=pl.Level.HOST):
        with pl.at(level=pl.Level.HOST, target=host):
            ring_stress_host(config)

@pl.function(level=pl.Level.HOST)
def ring_stress_host(config):
    """L3 kernel: fill and retire HOST-level ring, test pl.free()."""
    for i in range(ring_capacity):
        t = pl.alloc_tensor(4096)
        noop(t)             # L0 kernel
    if config.early_free:
        pl.free(t)

@pl.function(level=pl.Level.CORE)
def noop(t):
    """L0 kernel: fills buffer with sentinel for verification."""
    for i in range(len(t)): t[i] = 42.0
```

**Compiler output (hand-written C++ `.so` files):**

| `.so` file | Level | `linqu_orch_config().level` | Description |
|-----------|-------|-----------------------------|-------------|
| `kernel_noop.so` | L0 (CORE) | N/A (pure C) | Writes sentinel value, dispatched to `simpler` |
| `ring_phase1_L3_host.so` | L3 (HOST) | 3 | Phase 1: fill 8 tasks → retire → fill 8 again |
| `ring_phase2_L3_host.so` | L3 (HOST) | 3 | Phase 2: nested scopes with pl.free() |
| `ring_phase3_L4_pod.so` | L4 (POD) | 4 | Phase 3: Pod ring + dispatch L3 kernels to hosts |
| `ring_phase4_L6_cluster.so` | L6 (CLOS2) | 6 | Phase 4: L6→L5→L4→L3 nested ring allocation |
| `ring_phase4_L5_supernode.so` | L5 (CLOS1) | 5 | Phase 4: CLOS1-level ring allocation, dispatches L4 |
| `ring_phase4_L4_pod.so` | L4 (POD) | 4 | Phase 4: POD-level ring allocation, dispatches L3 |
| `ring_phase5_L4_pod.so` | L4 (POD) | 4 | Phase 5: Cross-level independence test |

Each phase is a self-contained PyPTO program at the appropriate hierarchy level.

### Step 10C.1: Test Design — The "Ring Stress" Program

**Action:** Design a program that creates a **carefully controlled workload** where each hierarchy level's rings experience:

1. **Allocation pressure:** more tasks than ring capacity → forces back-pressure.
2. **Scope nesting:** 3 depths at each level → tests multi-depth ring independence.
3. **Retirement:** scope exit retires slots → slots reused by next batch.
4. **`pl.free()` early release:** some tensors freed before scope exit → tests exactly-once token.
5. **Cross-level independence:** stalling a task at L4 does NOT block L3 retirement.

**Ring configuration (intentionally small to force wrap-around):**

```
Ring capacities per level (deliberately tight):
  task_ring[HOST][d]:      capacity = 8 slots   (at each depth d=0,1,2)
  task_ring[CLUSTER_0][d]: capacity = 4 slots
  task_ring[CLUSTER_1][d]: capacity = 4 slots
  task_ring[CLUSTER_2][d]: capacity = 2 slots

  buffer_ring[HOST][d]:      capacity = 32 KB
  buffer_ring[CLUSTER_0][d]: capacity = 16 KB
  buffer_ring[CLUSTER_1][d]: capacity = 16 KB
  buffer_ring[CLUSTER_2][d]: capacity = 8 KB
```

**Workload pattern (runs on 4 nodes in one pod for simplicity):**

```
Phase 1: Fill and retire at HOST level (L3)
══════════════════════════════════════════
  On host L3_0:
    scope depth 0:
      submit 8 tasks → fills task_ring[HOST][0] to capacity
      each task allocates 4 KB buffer → 32 KB total, fills buffer_ring
      wait for all 8 tasks to complete
    scope exit depth 0:
      retirement scan → all 8 slots reclaimed
      task_ring[HOST][0] back to 0 used
      buffer_ring[HOST][0] back to 0 used
    scope depth 0 again:
      submit 8 more tasks → verifies slots were reused (not stuck)
      → if ring was NOT properly retired, this would block forever
    scope exit → clean

Phase 2: Nested scopes at HOST level (L3)
══════════════════════════════════════════
  On host L3_0:
    scope depth 0:
      submit 4 tasks (T0..T3, consume 16 KB)
      scope depth 1:
        submit 8 tasks (T4..T11, fill task_ring[HOST][1])
        pl.free(T4.output)     → early release, task_freed=true
        pl.free(T5.output)     → early release
        scope exit depth 1:
          retirement scan for depth 1:
            T4, T5: skip (already freed), ref_count unchanged
            T6..T11: ref_count += 1 (scope token applied)
          → slots for T4..T11 reclaimable (ref_count == fanout_count)
          → depth 1 ring back to 0
      T0..T3 still alive at depth 0 (not affected by depth 1 exit)
    scope exit depth 0:
      T0..T3 retired
      buffer_ring[HOST][0] reclaimed

Phase 3: Pod-level ring (L4) with HOST-level nested inside
══════════════════════════════════════════════════════════
  Pod scope (L4):
    submit 4 pod-level tasks → uses task_ring[CLUSTER_0][0]
    allocates 4 × 4 KB = 16 KB in buffer_ring[CLUSTER_0][0]

    For each pod-task, dispatch to a HOST:
      HOST scope (L3):
        submit 8 HOST-level tasks → uses task_ring[HOST][0]
      HOST scope exit → HOST ring retired

    Pod scope exit:
      → task_ring[CLUSTER_0][0] retired
      → buffer_ring[CLUSTER_0][0] reclaimed
      → HOST rings unaffected (already retired)

Phase 4: Multi-level nesting (L6 → L5 → L4 → L3)
══════════════════════════════════════════════════════
  L6 scope (depth 0):
    submit 2 cluster tasks → task_ring[CLUSTER_2][0]
    L5 scope (depth 0):
      submit 4 supernode tasks → task_ring[CLUSTER_1][0]
      L4 scope (depth 0):
        submit 4 pod tasks → task_ring[CLUSTER_0][0]
        L3 scope (depth 0):
          submit 8 host tasks → task_ring[HOST][0]
        L3 scope exit → HOST ring retired
      L4 scope exit → POD ring retired (HOST unaffected)
    L5 scope exit → CLOS1 ring retired (POD, HOST unaffected)
  L6 scope exit → CLOS2 ring retired (all lower levels unaffected)

Phase 5: Cross-level independence (stall test)
══════════════════════════════════════════════════
  L4 scope:
    submit pod-task P0 (takes LONG to complete — simulate with sleep)
    L3 scope:
      submit 8 host tasks T0..T7
      T0..T7 all complete quickly
    L3 scope exit → HOST ring fully retired despite P0 still running
    P0 eventually completes
  L4 scope exit → POD ring retired

  Verify: HOST retirement happened BEFORE P0 completed.
  This proves layer-isolation invariant.
```

**Deliverable:** `examples/ring_stress_test/README.md` with this workload design.

### Step 10C.2: Ring State Snapshot API

**Action:** Implement a diagnostic API that captures the full state of all rings at any moment:

```cpp
struct RingSnapshot {
    Level level;
    uint16_t depth;
    // task_ring state
    size_t task_capacity;
    size_t task_used;
    size_t task_head;      // oldest live slot
    size_t task_tail;      // next allocation point
    // buffer_ring state
    size_t buffer_capacity_bytes;
    size_t buffer_used_bytes;
    size_t buffer_head;
    size_t buffer_tail;
    // retirement state
    size_t last_task_alive;
    // counters since last snapshot
    size_t alloc_count;
    size_t retire_count;
    size_t block_count;
    size_t free_count;     // pl.free calls
};

struct RingStackSnapshot {
    std::vector<RingSnapshot> layers;   // one per (level, depth)
    uint64_t timestamp_us;
};

RingStackSnapshot ring_stack_snapshot(const RingStack& stack);
```

Each daemon can dump snapshots at key moments (scope enter, scope exit, task complete) to `logs/ring_snapshots.jsonl` (JSON Lines, one snapshot per line).

**Deliverable:** `src/ring/ring_snapshot.h/.cpp`, integrated into `RingStack`.

### Step 10C.3: Ring Stress Kernels at Each Hierarchy Level

**Action:** Write the PyPTO kernel `.so` for each phase. Each is a hand-written C++ function that the compiler would emit for a `@pl.function(level=...)` block:
- Has a `linqu_orch_config()` that declares its hierarchy level.
- Uses `linqu_alloc_tensor`, `linqu_submit_task`, `LINQU_SCOPE`, `linqu_free_tensor`, `linqu_dump_ring_snapshot` through the ops table.
- Dispatches lower-level `.so` kernels via `linqu_submit_task`.

```cpp
// examples/ring_stress_test/kernels/ring_phase1_L3_host.cpp
// PyPTO compiler output for: @pl.function(level=pl.Level.HOST)
#include "linqu_orchestration_api.h"

extern "C" {

__attribute__((visibility("default")))
LinquOrchConfig linqu_orch_config(uint64_t* args, int arg_count) {
    (void)args; (void)arg_count;
    return LinquOrchConfig{.level = 3, .expected_arg_count = 0};
}

__attribute__((visibility("default")))
void linqu_orch_entry(LinquRuntime* rt, uint64_t* args, int arg_count) {
    int n = 1024;  // buffer size per task: 1024 floats = 4 KB

    // --- Round 1: fill to capacity and retire ---
    LINQU_SCOPE(rt) {
        for (int i = 0; i < 8; i++) {
            uint64_t h = linqu_alloc_tensor(rt, linqu_self(rt), n * sizeof(float));
            linqu_call_task(rt, linqu_self(rt), "kernel_noop.so",
                /*inputs=*/{}, /*outputs=*/{h},
                /*scalars=*/{(uint64_t)n}, /*tag=*/fmt("r1_t%d", i));
        }
    }
    // scope exit → 8 task slots + 32 KB retired
    linqu_dump_ring_snapshot(rt, "after_round1");

    // --- Round 2: reuse the same slots ---
    LINQU_SCOPE(rt) {
        for (int i = 0; i < 8; i++) {
            uint64_t h = linqu_alloc_tensor(rt, linqu_self(rt), n * sizeof(float));
            linqu_call_task(rt, linqu_self(rt), "kernel_noop.so",
                /*inputs=*/{}, /*outputs=*/{h},
                /*scalars=*/{(uint64_t)n}, /*tag=*/fmt("r2_t%d", i));
        }
    }
    linqu_dump_ring_snapshot(rt, "after_round2");
    // If slots were not properly reclaimed, round 2 would block.
}

// examples/ring_stress_test/kernels/ring_phase2_L3_host.cpp
// PyPTO compiler output for: @pl.function(level=pl.Level.HOST)
#include "linqu_orchestration_api.h"

extern "C" {

__attribute__((visibility("default")))
LinquOrchConfig linqu_orch_config(uint64_t* args, int arg_count) {
    (void)args; (void)arg_count;
    return LinquOrchConfig{.level = 3, .expected_arg_count = 0};
}

__attribute__((visibility("default")))
void linqu_orch_entry(LinquRuntime* rt, uint64_t* args, int arg_count) {
    int n = 1024;
    LINQU_SCOPE(rt) {  // depth 0
        uint64_t h_outer[4];
        for (int i = 0; i < 4; i++) {
            h_outer[i] = linqu_alloc_tensor(rt, linqu_self(rt), n * sizeof(float));
            linqu_call_task(rt, linqu_self(rt), "kernel_noop.so",
                {}, {h_outer[i]}, {(uint64_t)n}, fmt("outer_t%d", i));
        }
        linqu_dump_ring_snapshot(rt, "outer_4_submitted");

        LINQU_SCOPE(rt) {  // depth 1
            uint64_t h_inner[8];
            for (int i = 0; i < 8; i++) {
                h_inner[i] = linqu_alloc_tensor(rt, linqu_self(rt), n * sizeof(float));
                linqu_call_task(rt, linqu_self(rt), "kernel_noop.so",
                    {}, {h_inner[i]}, {(uint64_t)n}, fmt("inner_t%d", i));
            }
            // pl.free two inner tensors early
            linqu_free_tensor(rt, h_inner[0]);
            linqu_free_tensor(rt, h_inner[1]);
            linqu_dump_ring_snapshot(rt, "inner_8_submitted_2_freed");
        }
        // depth 1 scope exit: inner ring retired
        linqu_dump_ring_snapshot(rt, "after_inner_scope_exit");
        // outer 4 tasks still alive at depth 0
    }
    // depth 0 scope exit: outer ring retired
    linqu_dump_ring_snapshot(rt, "after_outer_scope_exit");
}
```

Also provide the L0-level PyPTO kernel (`@pl.function(level=pl.Level.CORE)`) that writes a sentinel for verification:

```cpp
// examples/ring_stress_test/kernels/kernel_noop.cpp
// PyPTO compiler output for: @pl.function(level=pl.Level.CORE)
// Pure C function — dispatched to simpler for execution
extern "C" void kernel_noop(float* out, int n) {
    for (int i = 0; i < n; i++) out[i] = 42.0f;
}
```

**Deliverable:** All `.so` sources in `examples/ring_stress_test/kernels/`:
- `kernel_noop.cpp` (L0)
- `ring_phase1_L3_host.cpp` (L3)
- `ring_phase2_L3_host.cpp` (L3)
- `ring_phase3_L4_pod.cpp` (L4)
- `ring_phase4_L6_cluster.cpp` (L6), `ring_phase4_L5_supernode.cpp` (L5), `ring_phase4_L4_pod.cpp` (L4)
- `ring_phase5_L4_pod.cpp` (L4)

### Step 10C.4: Verification Script — Ring Invariant Checker

**Action:** Write `examples/ring_stress_test/verify_rings.sh` that parses all `ring_snapshots.jsonl` files and checks invariants:

```bash
#!/bin/bash
BASE="${LINQU_BASE:-/tmp/linqu/ring_stress_test}"

echo "=== Multi-Level Ring Stress Test Verification ==="

# Collect all snapshot files
SNAPSHOTS=$(find "$BASE" -name "ring_snapshots.jsonl")

# The heavy checking is done by a purpose-built verifier
./build/linqu_ring_verifier --base "$BASE"
```

The `linqu_ring_verifier` program (C++) reads all snapshots and checks:

**Invariant 1 — No early free:**
```
For every snapshot: if a task slot is marked "reclaimable",
then ref_count == fanout_count for that slot.
```

**Invariant 2 — Exactly-once scope token:**
```
For tasks with task_freed == true:
  At scope exit, ref_count was NOT incremented (skip).
For tasks with task_freed == false:
  At scope exit, ref_count was incremented exactly once.
Total scope-tokens applied == (tasks_in_scope - tasks_already_freed).
```

**Invariant 3 — Layer isolation:**
```
At "after_inner_scope_exit" snapshot:
  task_ring[HOST][depth=1].task_used == 0  (inner retired)
  task_ring[HOST][depth=0].task_used == 4  (outer untouched)
```

**Invariant 4 — Deterministic behavior:**
```
Run the same program twice → identical ring_snapshots.jsonl sequences
(byte-for-byte identical after stripping timestamps).
```

**Invariant 5 — Task identity uniqueness:**
```
Across all snapshots: no two active tasks have the same TaskKey.
```

**Invariant 6 — Backward compatibility:**
```
A program that never calls pl.free() produces identical retirement
behavior to one where pl.free is never invoked.
(Run phase 1 with and without pl.free on a non-freed tensor → same result.)
```

**Additional ring-specific checks:**

| Check | Expected |
|-------|----------|
| **Reuse after retire:** After round 1 scope exit, `task_used == 0`. After round 2 fills 8, `task_used == 8`. | PASS if round 2 does not block. |
| **Buffer reclaim:** `buffer_used_bytes` returns to 0 after each scope exit. | PASS |
| **Peak usage tracking:** `peak_used` is monotonically non-decreasing within a run. | PASS |
| **Block count with tight ring:** If ring capacity == 8 and we submit 8 tasks, `block_count == 0`. If we try 9 before retirement, `block_count >= 1`. | PASS |
| **Cross-level independence (Phase 5):** HOST ring snapshot shows `task_used == 0` while POD ring shows `task_used > 0` (P0 still running). | PASS |
| **Depth independence:** Retiring depth 1 does not change `last_task_alive` for depth 0. | PASS |
| **pl.free idempotency:** Calling `linqu_free_tensor` twice on the same handle → `free_count` increments only once. | PASS |

**Deliverable:** `examples/ring_stress_test/verify_rings.sh`, `src/tools/linqu_ring_verifier.cpp`.

### Step 10C.5: Per-Level Ring Profile Output

**Action:** After each phase, each daemon writes `logs/ring_profile.json` following the design doc §12.3 format:

```json
{
    "node": {"l5": 0, "l4": 0, "l3": 0},
    "phases": [
        {
            "phase": "phase1_fill_retire",
            "layers": [
                {
                    "level": "HOST", "depth": 0,
                    "task_ring_capacity": 8,
                    "task_ring_peak_used": 8,
                    "task_ring_block_count": 0,
                    "buffer_ring_capacity_bytes": 32768,
                    "buffer_ring_peak_used_bytes": 32768,
                    "buffer_ring_block_count": 0,
                    "retire_scan_calls": 1,
                    "retire_scan_reclaimed_tasks": 8,
                    "retire_scan_reclaimed_bytes": 32768
                }
            ]
        },
        {
            "phase": "phase2_nested_free",
            "layers": [
                {
                    "level": "HOST", "depth": 0,
                    "task_ring_peak_used": 4,
                    "retire_scan_reclaimed_tasks": 4
                },
                {
                    "level": "HOST", "depth": 1,
                    "task_ring_peak_used": 8,
                    "retire_scan_reclaimed_tasks": 8,
                    "free_calls": 2,
                    "scope_tokens_applied": 6
                }
            ]
        },
        {
            "phase": "phase4_multi_level",
            "layers": [
                {"level": "CLUSTER_2", "depth": 0, "task_ring_peak_used": 2},
                {"level": "CLUSTER_1", "depth": 0, "task_ring_peak_used": 4},
                {"level": "CLUSTER_0", "depth": 0, "task_ring_peak_used": 4},
                {"level": "HOST",      "depth": 0, "task_ring_peak_used": 8}
            ]
        }
    ]
}
```

**Deliverable:** Profile integration in `NodeDaemon`.

**Acceptance:** Profile JSON has entries for every (level, depth) that was used, with non-zero metrics.

### Step 10C.6: Full Test Flow

**Action:** Write `examples/ring_stress_test/run_test.sh`:

```bash
#!/bin/bash
set -euo pipefail

BASE="/tmp/linqu/ring_stress_test"
rm -rf "$BASE"

echo "=== Phase 1: Fill and Retire — L3 HOST kernel (single host) ==="
LINQU_L3=0 LINQU_BASE="$BASE" ./build/linqu_daemon --run-once \
    --so examples/ring_stress_test/build/ring_phase1_L3_host.so \
    --ring-capacity task=8,buffer=32768

echo "=== Phase 2: Nested Scopes + pl.free — L3 HOST kernel (single host) ==="
LINQU_L3=0 LINQU_BASE="$BASE" ./build/linqu_daemon --run-once \
    --so examples/ring_stress_test/build/ring_phase2_L3_host.so \
    --ring-capacity task=8,buffer=32768

echo "=== Phase 3: Pod + Host — L4 POD kernel dispatching L3 HOST kernels ==="
for l3 in 0 1 2 3; do
    LINQU_L3=$l3 LINQU_L4=0 LINQU_BASE="$BASE" ./build/linqu_daemon &
done
sleep 1
./build/linqu_orchestrator --command run \
    --so examples/ring_stress_test/build/ring_phase3_L4_pod.so \
    --ring-capacity pod_task=4,host_task=8
kill $(jobs -p) 2>/dev/null; wait

echo "=== Phase 4: Multi-Level — L6→L5→L4→L3 kernel chain ==="
for l3 in 0 1 2 3; do
    LINQU_L5=0 LINQU_L4=0 LINQU_L3=$l3 LINQU_BASE="$BASE" ./build/linqu_daemon &
done
sleep 1
./build/linqu_orchestrator --command run \
    --so examples/ring_stress_test/build/ring_phase4_L6_cluster.so \
    --ring-capacity clos2_task=2,clos1_task=4,pod_task=4,host_task=8
kill $(jobs -p) 2>/dev/null; wait

echo "=== Phase 5: Cross-Level Independence — L4 POD kernel ==="
for l3 in 0 1 2 3; do
    LINQU_L5=0 LINQU_L4=0 LINQU_L3=$l3 LINQU_BASE="$BASE" ./build/linqu_daemon &
done
sleep 1
./build/linqu_orchestrator --command run \
    --so examples/ring_stress_test/build/ring_phase5_L4_pod.so
kill $(jobs -p) 2>/dev/null; wait

echo ""
echo "=== Verification ==="
./examples/ring_stress_test/verify_rings.sh
./build/linqu_ring_verifier --base "$BASE"

echo ""
echo "=== Ring Stress Test Complete ==="
```

**Deliverable:** `examples/ring_stress_test/run_test.sh`.

**Acceptance:** All 5 phases run without deadlock. `linqu_ring_verifier` reports all invariants hold. Profile JSON confirms expected peak usage and reclamation counts.

---

## Milestone 11: Build System for arm64

**Goal:** Compile the `linqu_daemon` and `linqu_orchestrator` executables for arm64.

### Step 11.1: CMake Build

**Action:** Write the project `CMakeLists.txt` that:

1. Compiles `linqu_daemon` — the node daemon executable.
2. Compiles `linqu_orchestrator` — the top-level orchestrator executable.
3. Compiles the runtime libraries (`src/core/`, `src/ring/`, `src/scope/`, `src/transport/`, `src/discovery/`, `src/dispatch/`, `src/daemon/`, `src/profiling/`).
4. Targets aarch64 with C++17.

**Deliverable:** `CMakeLists.txt`.

**Acceptance:** `cmake .. && make` produces `linqu_daemon` and `linqu_orchestrator`. `file linqu_daemon` shows `ELF 64-bit LSB executable, ARM aarch64`.

### Step 11.2: Verify Daemon Launches

**Action:** Test that a single `linqu_daemon` process starts, writes `identity.json`, and exits cleanly on `SIGTERM`.

**Deliverable:** `tests/e2e/test_single_daemon.sh`.

---

## Milestone 12: Multi-Process Verification Environment

**Goal:** Build the launch infrastructure that creates the simulated cluster as multiple processes with on-disk storage.

### Step 12.1: Per-Node Storage Directory Layout (1024-Node Cluster)

**Action:** The filesystem layout for the 1024-node virtual cluster:

```
/tmp/linqu/linqu_vcluster_1024/
├── L6_0/
│   ├── L5_0/                          # Supernode 0 (64 hosts)
│   │   ├── L4_0/                      # Pod 0 (16 hosts)
│   │   │   ├── L3_0/
│   │   │   │   ├── daemon.sock        # Unix domain socket
│   │   │   │   ├── daemon.pid         # Process PID
│   │   │   │   ├── code_cache/        # Received .so files
│   │   │   │   │   ├── <hash1>.so
│   │   │   │   │   └── <hash2>.so
│   │   │   │   ├── data_cache/        # Received data buffers
│   │   │   │   └── logs/
│   │   │   │       └── daemon.log     # Per-node log
│   │   │   ├── L3_1/ ... L3_15/       # 16 hosts per pod
│   │   ├── L4_1/ ... L4_3/            # 4 pods per supernode
│   ├── L5_1/ ... L5_15/               # 16 supernodes
├── cluster.pids                        # All 1024 PIDs
└── orchestrator.log
```

Total: **4178 directories** (1024 host dirs × 3 subdirs each + hierarchy dirs). Verified creation time: **459ms**.

**Action:** Implement `create_node_storage(base_path, coordinate)`:
- Creates the full directory path.
- Creates `code_cache/`, `data_cache/`, `logs/` subdirectories.
- Returns the storage path.

**Deliverable:** `src/daemon/storage.h/.cpp`, unit tests.

**Acceptance:** `create_node_storage("/tmp/linqu/test", {l6=0, l5=7, l4=2, l3=11})` creates `/tmp/linqu/test/L6_0/L5_7/L4_2/L3_11/` with subdirectories.

### Step 12.2: Cluster Launcher Scripts (in `venv/`)

**Action:** The cluster launch infrastructure is pre-built in `venv/`:

| Script | Purpose |
|--------|---------|
| `venv/cluster_config.json` | Cluster topology definition (16 L5 × 4 L4 × 16 L3 = 1024 hosts) |
| `venv/cluster_topology.md` | Human-readable topology documentation |
| `venv/create_topology.sh` | Creates the 4178-directory filesystem structure (verified: 459ms) |
| `venv/launch_cluster.sh` | Launches 1024 `linqu_daemon` processes in batches of 64 |
| `venv/shutdown_cluster.sh` | Graceful shutdown of all daemons (SIGTERM → SIGKILL fallback) |
| `venv/cluster_status.sh` | Real-time cluster health report with per-L5 breakdown |

The `launch_cluster.sh` script:
1. Cleans previous state.
2. Creates directory structure for 1024 nodes.
3. Launches 1024 `linqu_daemon` processes (batches of 64, with progress reporting).
4. Each process receives its coordinate via env vars: `LINQU_L6=0 LINQU_L5=<s> LINQU_L4=<p> LINQU_L3=<h>`.
5. Waits for all `daemon.sock` files to appear (up to 30s timeout).
6. Saves all PIDs to `cluster.pids` for clean shutdown.

To run the identity + topology verification test on the 1024-node cluster:

```bash
# Step 1: Create directory structure (no daemons yet)
./venv/create_topology.sh --verify

# Step 2: Launch 1024 daemon processes
./venv/launch_cluster.sh

# Step 3: Check cluster health
./venv/cluster_status.sh

# Step 4: Trigger topology discovery on all daemons
./build/linqu_orchestrator --command discover

# Step 5: Verify all identity dumps and discovery results
./examples/topology_test/verify_topology.sh

# Step 6: Shut down
./venv/shutdown_cluster.sh
```

The test verifies:
- All 1024 daemons start and write `identity.json` with correct coordinates.
- All daemons discover their peers: 16 in same pod, 64 in same supernode, 1024 cluster-wide.
- All socket files exist and are reachable.
- No dead peers, no duplicate indices, no missing nodes.

**Deliverable:** `venv/launch_cluster.sh`, `venv/shutdown_cluster.sh`, `venv/cluster_status.sh`, `examples/topology_test/verify_topology.sh`.

**Acceptance:** `verify_topology.sh` reports all 8 checks PASS on the 1024-node cluster.

### Step 12.3: `linqu_orchestrator` Executable

**Action:** Implement the orchestrator program that drives the top-level execution:

```cpp
// src/daemon/orchestrator_main.cpp
int main(int argc, char** argv) {
    // Parse args: --command [discover|status|shutdown]
    // 1. Read coordinates from env
    // 2. Initialize transport, discovery, PeerRegistry
    // 3. Discover all daemon processes via filesystem scan
    // 4. If --command discover:
    //    - Send DISCOVER message to all daemons
    //    - Wait for all to write discovery.json
    // 5. If --command status:
    //    - Collect and display cluster health summary
    // 6. Print summary
}
```

**Deliverable:** `src/daemon/orchestrator_main.cpp`, builds to `linqu_orchestrator`.

---

## Milestone 13: Profiling Framework (`src/profiling/`)

**Goal:** Instrument ring buffers with per-layer metrics.

### Step 13.1: `RingMetrics` Struct

**Action:** Define per-(level, depth) metrics.

**Deliverable:** `src/profiling/ring_metrics.h`.

### Step 13.2: Instrument `RingBuffer`

**Action:** Add metric collection hooks.

**Deliverable:** Modified `src/ring/ring_buffer.h`, unit tests.

### Step 13.3: JSON Profiling Output

**Action:** Implement `ProfileReport::to_json()`.

**Deliverable:** `src/profiling/profile_report.h`, unit test.

### Step 13.4: Per-Node Profiling Logs

**Action:** Each daemon writes its ring metrics to `<storage_path>/logs/ring_profile.json` on scope exit or shutdown. The orchestrator aggregates all per-node profiles into a single cluster-wide report.

**Deliverable:** Integration into `NodeDaemon` shutdown path.

**Acceptance:** After `run_cluster.sh`, each node's `logs/ring_profile.json` exists and contains valid metrics.

---

## Milestone 14: End-to-End Integration Tests

**Goal:** Validate the identity dump, topology discovery, and IPC infrastructure at increasing scale.

### Step 14.1: Single Daemon Test

**Action:** Launch 1 daemon process. Verify:
- `identity.json` is written with correct fields.
- Socket file exists and accepts connections.
- Responds to `HEARTBEAT` message.
- Clean shutdown on `SIGTERM` removes socket file.

**Deliverable:** `tests/e2e/test_single_daemon.sh`.

### Step 14.2: Single Pod Test (16 Hosts)

**Action:** Launch 16 daemon processes in one pod (l5=0, l4=0, l3=0..15).
- All 16 write `identity.json`.
- Trigger discovery.
- Each discovers 15 same-pod peers.
- Verify via `verify_topology.sh` subset.

**Deliverable:** `tests/e2e/test_single_pod.sh`.

**Acceptance:** All 16 daemons report `same_pod_count: 16`.

### Step 14.3: Single Supernode Test (64 Hosts)

**Action:** Launch 64 daemon processes in one supernode (l5=0, l4=0..3, l3=0..15).
- All 64 write `identity.json`.
- Trigger discovery.
- Each discovers 16 same-pod peers and 64 same-supernode peers.
- Verify cross-pod discovery: node (l4=0, l3=5) can discover node (l4=2, l3=11).

**Deliverable:** `tests/e2e/test_single_supernode.sh`.

**Acceptance:** All 64 daemons report correct pod and supernode counts.

### Step 14.4: Full Cluster Test (1024 Hosts)

**Action:** Launch all 1024 daemons via `launch_cluster.sh`. Run full topology verification:
- All 1024 `identity.json` files exist.
- All 1024 `discovery.json` files pass the 8 checks.
- Discovery time per node is reasonable (< 5 seconds).

**Deliverable:** `examples/topology_test/run_test.sh`.

**Acceptance:** `verify_topology.sh` reports all 8 checks PASS.

### Step 14.5: IPC Round-Trip Test

**Action:** Launch 2 daemon processes. From the orchestrator:
1. Send `HEARTBEAT` to daemon A → verify response.
2. Send `REG_CODE` (a dummy 1-byte payload) to daemon B → verify it appears in B's `code_cache/`.
3. Send `CALL_TASK` referencing the registered code → verify B responds with `TASK_COMPLETE`.
4. Send `SCOPE_EXIT` → verify B processes it.

This validates the full message protocol over Unix sockets without any actual computation.

**Deliverable:** `tests/e2e/test_ipc_roundtrip.sh`.

### Step 14.6: Tensor DAG Test (4-Node Pod)

**Action:** Run `examples/tensor_dag_test/run_test.sh`:
- Launch 4 daemons in one pod.
- Execute the 5-task distributed DAG: `f = (a+b+1)(a+b+2) + (a+b)`.
- Verify: numerical correctness, cross-node transfers, dependency graph, scope retirement.

**Deliverable:** `tests/e2e/test_dag_pod.sh`.

**Acceptance:** All 6 checks in `verify_dag.sh` pass.

### Step 14.7: Tensor DAG Test (Full Cluster)

**Action:** Run the DAG test across all 1024 hosts (64 pods, each running the 5-task DAG independently).
- 320 total tasks.
- Verify concatenated output for all 65536 elements.
- Verify per-pod task traces show correct dependencies.

**Deliverable:** `tests/e2e/test_dag_cluster.sh`.

**Acceptance:** Numerical verification and all per-pod traces pass.

### Step 14.8: Ring Stress Test (Single Host)

**Action:** Run phases 1–2 of the ring stress test on a single daemon:
- Phase 1: Fill 8 task slots → retire → fill 8 again (reuse).
- Phase 2: Nested scopes with `pl.free()` on 2 inner tensors.
- Verify: ring snapshots show correct `task_used`, `buffer_used_bytes`, `free_count`.
- Verify: all 6 invariants hold via `linqu_ring_verifier`.

**Deliverable:** `tests/e2e/test_ring_single_host.sh`.

### Step 14.9: Ring Stress Test (Multi-Level)

**Action:** Run phases 3–5 on 4 daemons:
- Phase 3: Pod-level ring + nested host-level rings.
- Phase 4: L6→L5→L4→L3 four-level nesting.
- Phase 5: Cross-level independence (stalled pod task doesn't block host retirement).
- Verify: per-level ring profiles show expected peak usage.
- Verify: HOST ring retired before POD task completed (phase 5 timestamp check).

**Deliverable:** `tests/e2e/test_ring_multi_level.sh`.

**Acceptance:** `linqu_ring_verifier` passes all invariants. Phase 5 proves layer isolation.

### Step 14.10: Node Failure Test

**Action:** Launch 16 daemons (one pod). Kill one daemon (`SIGKILL`).
- Remaining 15 nodes eventually report the killed node as `"dead"` (or socket unreachable).
- Re-discovery shows 15 live peers instead of 16.
- No other nodes crash or hang.

**Deliverable:** `tests/e2e/test_node_failure.sh`.

### Step 14.11: Forward-Compatibility Smoke Test

**Action:** Unit tests that create `TaskKey` and `LinquCoordinate` with non-zero L4–L6 fields. Serialize to `LinquHeader` format. Deserialize and verify. PeerRegistry queries with full coordinates return correct results.

**Deliverable:** `tests/forward_compat/test_7_level_smoke.cpp`.

---

## Milestone 15: Documentation and First Commit

**Goal:** Produce a clean, documented, tested first version.

### Step 15.1: README.md

**Action:** Write a project README covering:
- What the project is (Linqu runtime for Level 3–6, self-contained).
- Relationship to `simpler` (separate codebase for L0–2, not modified or linked; future `ChipBackend` adapter for Tier 2 integration).
- The verification environment (multi-process on single arm64 machine).
- How to build and run the hierarchical test.
- Architecture overview (link to design doc).

### Step 15.2: API Reference

**Action:** Write `docs/api_reference.md` documenting:
- `linqu_orchestration_api.h` — for writing orchestration `.so` files at L3–L6.
- `LinquOrchestratorState` — unified runtime for all levels (L3–L6).
- `LinquDispatcher` / `LocalDispatcher` / `RemoteDispatcher` — level-dependent dispatch.
- `NodeDaemon` — process management.
- `Transport` — IPC/network abstraction.

### Step 15.3: Push to GitHub

**Action:** `git add`, commit, push to `hengliao1972/pypto_runtime_distributed`.

---

## Phase 0 Summary: What Is Delivered

| Component | Description | Status |
|-----------|-------------|--------|
| **Core Infrastructure** | | |
| `pl::Level` enum | All 7 levels, 17 aliases | Complete |
| `LinquCoordinate` | 7-level coordinate with `to_path()` for filesystem | Complete |
| `TaskKey` | Full hierarchical task identity | Complete |
| **Unified Runtime (same code at every level)** | | |
| `linqu_orchestration_api.h` | **Single** ops-table header for ALL levels L0–L6 (unified `LinquRuntimeOps`) | Complete |
| `LinquRuntime` / `LinquRuntimeOps` | Unified ops table: `submit_task`, `scope_begin/end`, `alloc_tensor`, `free_tensor`, `query_peers`, `wait_all`, logging — used by every `.so` | Complete |
| `LinquOrchConfig` | Config struct with level tag, every `.so` exports `linqu_orch_config()` | Complete |
| `LinquParam` | Parameter descriptor (INPUT/OUTPUT/INOUT/SCALAR) mirroring `PTOParam` | Complete |
| `LINQU_SCOPE(rt)` | RAII scope guard macro, identical pattern to `PTO2_SCOPE(rt)` | Complete |
| `LinquOrchestratorState` | **One class, all levels**: ring buffers + tensor map + scope stack + dispatcher (mirrors `PTO2OrchestratorState`) | Complete |
| `LinquHeapRing` | Buffer allocation ring — same algorithm as `PTO2HeapRing`, local backing | Complete |
| `LinquTaskRing` | Task slot ring — same algorithm as `PTO2TaskRing`, local backing | Complete |
| `LinquDepListPool` | Dependency list pool — same algorithm as `PTO2DepListPool` | Complete |
| `LinquTensorMap` | Tensor→producer map — same core as `PTO2TensorMap` + cross-node lookup extension | Complete |
| Scope stack | `scope_tasks[]`, `scope_begins[]`, `scope_stack_top` — same as simpler | Complete |
| **Level-Dependent Dispatchers** | | |
| `LocalDispatcher` | L3: local execution, stub to L2 (future: ChipBackend adapter) | Complete |
| `RemoteDispatcher` | L4–L6: sends tasks to child nodes via IPC/network | Complete |
| `MockDispatcher` | Testing: simulates execution with configurable latency | Complete |
| **Communication** | | |
| `LinquHeader` | 24-byte wire format with all level fields | Complete |
| Message protocol | 7+ message types with payload serialization | Complete |
| `UnixSocketTransport` | IPC for multi-process verification | Complete |
| `PeerRegistry` | Thread-safe membership table | Complete |
| `FilesystemDiscovery` | Discover daemon processes via socket files | Complete |
| **Executables** | | |
| `NodeDaemon` | Per-process daemon: loads `.so`, creates `LinquOrchestratorState`, runs kernel | Complete |
| `linqu_daemon` | Standalone daemon executable for arm64 | Complete |
| `linqu_orchestrator` | Top-level driver: loads L6 `.so`, dispatches downward | Complete |
| **Virtual Cluster** | | |
| 1024-node virtual cluster | `venv/` scripts: create_topology, launch, status, shutdown | Complete |
| Per-node storage | Filesystem dirs with code_cache, data_cache, logs per node | Complete |
| **Test Program 1: Topology (4-level kernel stack, all using unified API)** | | |
| `topo_L6_cluster.so` | L6 kernel: `query_peers(5)` + `submit_task` to 16 supernodes | Complete |
| `topo_L5_supernode.so` | L5 kernel: `query_peers(4)` + `submit_task` to 4 pods | Complete |
| `topo_L4_pod.so` | L4 kernel: `query_peers(3)` + `submit_task` to 16 hosts | Complete |
| `topo_L3_host.so` | L3 kernel: `self_coord` + `query_peers` + `log_info` | Complete |
| `verify_topology.sh` | 8-check automated verification of 1024-node cluster | Complete |
| **Test Program 2: Distributed DAG (L0+L4+L5+L6 kernel stack)** | | |
| `kernel_add.so`, `kernel_add_scalar.so`, `kernel_mul.so` | L0 kernels: stubbed dispatch in Phase 0 | Complete |
| `dag_L4_pod.so` | L4 kernel: 5-task DAG `(a+b+1)(a+b+2)+(a+b)` across 4 hosts | Complete |
| `dag_L5_supernode.so`, `dag_L6_cluster.so` | L5/L6 kernels: hierarchical dispatch for cluster-scale DAG | Complete |
| `LinquTensorMap` cross-node | Remote lookup + data transfer for cross-node dependencies | Complete |
| `verify_dag.sh` | 6-check: numerical, transfers, deps, scopes, lifetime, completeness | Complete |
| Per-task execution trace | `task_trace.json` per node: inputs, producers, transfers, scopes | Complete |
| **Test Program 3: Ring Stress (L0+L3+L4+L5+L6 kernel stack)** | | |
| `kernel_noop.so` | L0 kernel: sentinel-writing no-op (stubbed dispatch in Phase 0) | Complete |
| `ring_phase1_L3_host.so`, `ring_phase2_L3_host.so` | L3 kernels: fill/retire and nested scopes with pl.free() | Complete |
| `ring_phase3_L4_pod.so` | L4 kernel: pod ring + dispatches L3 kernels | Complete |
| `ring_phase4_L6/L5/L4.so` | L6/L5/L4 kernels: 4-level nested ring allocation | Complete |
| `ring_phase5_L4_pod.so` | L4 kernel: cross-level independence test | Complete |
| Ring snapshot API | `RingStackSnapshot` diagnostic capture at scope enter/exit/free | Complete |
| `linqu_ring_verifier` | Automated checker for all 6 ring correctness invariants | Complete |
| Ring profiling | Per-layer metrics with JSON output, per-phase breakdown | Complete |
| **End-to-End Tests** | | |
| E2E tests | daemon, pod, supernode, cluster, IPC, DAG, ring-stress, failure | Complete |

---

## Phases 1–5: Summary (Detailed Plans to Follow)

| Phase | Trigger | Key Work |
|-------|---------|----------|
| **Phase 1** | `simpler` integration ready + chip hardware available | **Tier 2 bridge implementation:** Implement the `ChipBackend` adapter within `pypto_runtime_distributed` that dynamically links to `simpler`'s `libhost_runtime.so` (via `dlopen`, no compile-time dependency). Implement `h2d_copy`/`d2h_copy`; handle→device GM address mapping; device GM buffer lifecycle management. Does NOT modify `simpler`. |
| **Phase 2** | Multi-host pod hardware ready | Replace `UnixSocketTransport` with TCP sockets; activate gossip (UDP heartbeat, SWIM); real SPMD fan-out. |
| **Phase 3** | Supernode hardware ready | Activate L5 rings; hierarchical dispatch (L5 leaders); aggregation nodes. |
| **Phase 4** | Full cluster hardware ready | Activate L6 rings; bandwidth-aware scheduling; RDMA optimization. |
| **Phase 5** | Production deployment | Full profiling; CI gating; stress tests; compatibility adapters. |

Detailed implementation plans for Phases 1–5 will be written when their hardware triggers are met.

---

## Appendix: Three-Tier Communication Model Reference

This appendix summarizes the three-tier communication architecture that governs all cross-level data flow in the Linqu system. See `linqu_runtime_design.md` §7.4 for the full specification.

| Tier | Levels | Memory Model | Transport | Sync Mechanism | Managed By |
|------|--------|-------------|-----------|----------------|------------|
| **1** | L0–L2 (intra-chip) | Shared device GM | Direct pointer access | `__atomic` CAS, LOAD_ACQUIRE/STORE_RELEASE | `simpler` (separate codebase, not modified or linked) |
| **2** | L3↔L2 (host↔device) | Host mem ↔ Device GM | `h2d_copy` / `d2h_copy` DMA | DMA completion events, MMIO | Future `ChipBackend` adapter (in `pypto_runtime_distributed`) |
| **3** | L4–L6↔L3 (inter-process) | Independent address spaces | Unix socket / TCP / RDMA | Message ACK (TASK_COMPLETE) | `RemoteDispatcher` + `NodeDaemon` |

Key design decisions driven by this model:
- `LinquTensorMap` uses opaque handles (not GM addresses) because L3–L6 have no shared buffer space.
- `LinquOrchestratorState` uses `std::mutex` (not atomics) because all state is process-local.
- Back-pressure is message-driven (spin on `TASK_COMPLETE` arrivals), not shared-memory-driven.
- The `NodeDaemon` at L3 is the bridge between Tier 3 (messages from L4+) and Tier 2 (DMA to L2).
