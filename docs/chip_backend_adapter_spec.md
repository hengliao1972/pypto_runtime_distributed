# ChipBackend Adapter Specification

## Status: Design Only (Phase 0)

This document specifies the interface for the future `ChipBackend` adapter that bridges the L3↔L2 boundary. The adapter is NOT implemented in Phase 0; the `LocalDispatcher` uses a stub instead.

## Core Principle: L2 Is a Self-Contained Runtime Entity

L2 (Chip) is NOT just a piece of hardware — it is a complete runtime unit that includes:
- Host-side driver code (`DeviceRunner`, `pto_runtime_c_api`)
- AICPU scheduler (on-device, manages ready queue and dispatch)
- AICore workers (on-device, execute kernels)
- Its own h2d/d2h DMA capability
- Its own GM allocation and lifecycle management

**L3 does NOT manage device GM, does NOT perform h2d/d2h DMA, and does NOT allocate device-side buffers.** L3 treats L2 as a black box that can autonomously execute tasks given host memory addresses.

## Architecture

```
L4+ (Tier 3: message passing via IPC)
  └──► L3 NodeDaemon (host CPU process)
         │
         ├── LinquOrchestratorState (DAG, rings, tensormap)
         │     alloc_tensor → allocates HOST MEMORY (not GM)
         │     submit_task / submit_task_group → passes host addresses to L2
         │
         └──► ChipBackend (L3 Platform layer, within pypto_runtime_distributed)
                │
                ├── Task dispatch: triggers L2's own host-side entry point
                ├── P2P mapping: cross-chip GM address space mapping (multi-chip only)
                │
                ▼
         L2 Chip (self-contained runtime)
           │  Receives: host memory addresses + kernel binary
           │  Internally performs:
           │    1. h2d_copy(host_addr → device GM)
           │    2. simpler runtime lifecycle (create → init → launch → finalize)
           │    3. d2h_copy(device GM → host_addr)
           │  Reports: completion status back to L3
           │
           └──► Device GM (L0-L2 simpler runs here)
                  AICPU scheduler → AICore workers
```

## L3 ↔ L2 Communication: Host Memory (Zero-Copy)

L3 and L2 share the same host address space (same physical machine). Data exchange uses **host memory** as the medium:

```
L3 orchestrator                              L2 chip runtime
─────────────                                ────────────────
alloc_tensor(size) → host_addr
submit_task(host_addr, kernel.so) ──────►    receives (host_addr, kernel.so)
                                              h2d_copy(host_addr → GM)
                                              run simpler (all in GM)
                                              d2h_copy(GM → host_addr)
                                     ◄────── notify_completion
group COMPLETED when all subs done
```

The host memory buffer can be zero-copy between L3 and L2 — L2's host-side code directly reads/writes the address L3 allocated. No intermediate serialization or copy is needed at the L3↔L2 boundary.

## Interface

```cpp
class ChipBackend : public LinquDispatcher {
public:
    // Initialize: dlopen simpler's libhost_runtime.so, enumerate available chips
    bool init(const std::string& simpler_lib_path, int num_chips);

    // --- LinquDispatcher interface ---

    // Dispatch a task to a specific chip.
    // params contain host memory addresses — L2 handles h2d/d2h internally.
    bool dispatch(int32_t task_id,
                  const LinquCoordinate& target,
                  const std::string& kernel_so,
                  LinquParam* params, int num_params) override;

    void wait_all() override;

    // --- Multi-chip P2P (Platform capability) ---

    // Map a region of chip_a's GM into chip_b's address space (and vice versa).
    // After this call, chip_b can read/write the mapped address as if it were local GM.
    // This is an L3 Platform capability — simpler is unaware of the mapping.
    bool setup_p2p_mapping(int chip_a, int chip_b,
                           uint64_t gm_base, size_t size);

    // Tear down a previously established P2P mapping.
    void teardown_p2p_mapping(int chip_a, int chip_b,
                              uint64_t gm_base, size_t size);

    void shutdown();
};
```

## Key Responsibilities

### 1. Task Dispatch (Trigger L2's Own Lifecycle)

ChipBackend does NOT orchestrate L2 internals. It triggers L2's host-side entry point and passes parameters:

```cpp
bool ChipBackend::dispatch(int32_t task_id, const LinquCoordinate& target,
                            const std::string& kernel_so,
                            LinquParam* params, int num_params) {
    int chip_id = target.l2_idx;

    // Trigger L2's host-side runtime lifecycle.
    // L2 autonomously handles: h2d → execute → d2h.
    simpler_create_runtime_fn_(&chip_contexts_[chip_id]);
    simpler_init_runtime_fn_(chip_contexts_[chip_id], kernel_so, params, num_params);
    simpler_launch_runtime_fn_(chip_contexts_[chip_id]);
    simpler_finalize_runtime_fn_(chip_contexts_[chip_id]);

    // Notify L3 orchestrator that this task is done
    notify_completion({task_id, TaskStatus::COMPLETED, 0});
    return true;
}
```

### 2. Dynamic Linking (No Compile-Time Dependency)

The adapter loads simpler's `libhost_runtime.so` via `dlopen`/`dlsym` at runtime. This ensures `pypto_runtime_distributed` has zero compile-time dependency on `simpler`.

```cpp
bool ChipBackend::init(const std::string& simpler_lib_path, int num_chips) {
    lib_handle_ = dlopen(simpler_lib_path.c_str(), RTLD_NOW);
    simpler_create_runtime_fn_  = (CreateFn)dlsym(lib_handle_, "create_runtime");
    simpler_init_runtime_fn_    = (InitFn)dlsym(lib_handle_, "init_runtime");
    simpler_launch_runtime_fn_  = (LaunchFn)dlsym(lib_handle_, "launch_runtime");
    simpler_finalize_runtime_fn_ = (FinalizeFn)dlsym(lib_handle_, "finalize_runtime");
    // ...
}
```

### 3. P2P GM Mapping (Multi-Chip Group Tasks)

When `submit_task_group` dispatches to multiple chips as one logical worker, L3 uses its **Platform capability** to establish cross-chip GM mappings before dispatching sub-tasks:

```
L3 orchestrator kernel code:
  ┌─────────────────────────────────────────────┐
  │ // Two chips cooperating on a matmul         │
  │ setup_p2p_mapping(chip_0, chip_1, base, sz); │
  │                                               │
  │ LinquSubTaskSpec subs[2] = {                  │
  │   { chip_0, "shard.so", &p0, 1 },            │
  │   { chip_1, "shard.so", &p1, 1 },            │
  │ };                                            │
  │ submit_task_group(rt, "shard.so", gp, 2,      │
  │                   subs, 2);                    │
  └─────────────────────────────────────────────┘

Each chip:
  - h2d_copy its own input shard from host memory
  - Compute locally in GM
  - Read/write partner's GM via P2P mapping (looks like local address to simpler)
  - d2h_copy its output shard to host memory
  - Report completion

L3 group aggregation:
  - Both sub-tasks complete → group COMPLETED → downstream tasks become ready
```

Simpler sees the P2P-mapped addresses as ordinary local GM — no code change needed.

### 4. No Modification to `simpler`

All adapter logic lives in `pypto_runtime_distributed`. Simpler's source code is not modified. The interaction is through simpler's existing stable C API.

## What ChipBackend Does NOT Do

| Responsibility | Owner | NOT ChipBackend |
|---------------|-------|-----------------|
| h2d_copy (host → device GM) | L2's own host-side code | -- |
| d2h_copy (device GM → host) | L2's own host-side code | -- |
| Device GM allocation | L2's `DeviceRunner::allocate_tensor()` | -- |
| Device GM lifecycle | L2's scope/ring management | -- |
| AICPU/AICore scheduling | L2's `AicpuExecutor` | -- |
| Handle-to-GM-address mapping | Not needed (L3 passes host addrs) | -- |

## Phased Implementation

| Phase | Content | Scope |
|-------|---------|-------|
| **Phase 0** (current) | `LocalDispatcher` stub — immediate completion | `pypto_runtime_distributed` only |
| **Phase 1a** | Single-chip dispatch: ChipBackend triggers L2 lifecycle via dlopen | `pypto_runtime_distributed` new file; simpler unchanged |
| **Phase 1b** | Multi-chip group: P2P GM mapping + `submit_task_group` integration | `pypto_runtime_distributed` ChipBackend extension |
| **Phase 2** | Async dispatch: thread pool for concurrent multi-chip execution | ChipBackend internal refactor |
| **Phase 3** | Pipeline overlap: L3 orchestrator submits next batch while L2 executes | May require simpler to expose stream/async API |

## Constraints

- The adapter lives entirely within `pypto_runtime_distributed`
- Simpler is a separate, immutable codebase — zero source modifications
- L3 tensors are host memory — L2 is responsible for all device memory management
- P2P GM mapping is an L3 Platform capability, not a simpler runtime feature
