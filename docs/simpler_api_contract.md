# Simpler API Contract — Read-Only Reference

This document describes the `simpler` runtime API surface for future reference. The Linqu distributed runtime (`pypto_runtime_distributed`) does NOT link against or call any of these APIs in Phase 0. This is a read-only reference for the future `ChipBackend` adapter.

## Overview

The `simpler` runtime handles Levels 0–2 (Core, Chip Die, Chip) of the Linqu hierarchy. It operates entirely in device Global Memory (GM) using atomic operations and memory barriers for synchronization.

## Key Types

### `PTO2RuntimeOps`
Function-pointer table exposed to orchestration `.so` files:
- `submit_task(rt, kernel_id, worker_type, params, num_params)` — submit a task to a worker
- `scope_begin(rt)` / `scope_end(rt)` — scope management
- `orchestration_done(rt)` — signal orchestration completion

### `PTO2Runtime*`
Opaque runtime context pointer passed to the orchestration `.so` entry function.

### `PTO2OrchestratorState`
Owns ring buffers (task ring, heap ring), tensor map, scope stack, and statistics. All data structures reside in device GM.

### `PTO2SchedulerState`
Device-side scheduler that dispatches ready tasks to hardware workers (CUBE, VECTOR, AI_CPU, ACCELERATOR). Uses atomic `fanin_refcount`/`fanout_refcount` arrays and task state machine (PENDING→READY→RUNNING→COMPLETED→CONSUMED).

## Entry Point Signatures

```c
// Standard orchestration .so entry
void aicpu_orchestration_entry(PTO2Runtime* rt, uint64_t* args, int arg_count);

// Configuration export
PTO2OrchestrationConfig aicpu_orchestration_config(uint64_t* args, int arg_count);
```

## Shared Memory Interface

### `PTO2SharedMemoryHeader`
Shared between host CPU and device. Contains:
- `current_task_index` — written by orchestrator (STORE_RELEASE), read by scheduler (LOAD_ACQUIRE)
- `last_task_alive` — written by scheduler (STORE_RELEASE), read by orchestrator (LOAD_ACQUIRE)
- Ring buffer base addresses and capacities

## h2d/d2h DMA API

Data crosses the host↔device boundary via:
- **`h2d_copy(host_addr, device_addr, size)`** — host-to-device DMA transfer
- **`d2h_copy(device_addr, host_addr, size)`** — device-to-host DMA transfer

These are hardware-specific operations provided by the device driver. The future `ChipBackend` adapter will call them to transfer tensor data between Linqu's host memory and `simpler`'s device GM.

## Ring Buffer Types

| Type | Description |
|------|-------------|
| `PTO2HeapRing` | O(1) bump allocation in device GM with wrap-around |
| `PTO2TaskRing` | Fixed-size task descriptor ring with back-pressure |
| `PTO2DepListPool` | Linked-list pool for dependency edges |
| `PTO2TensorMap` | Address-based hash table with multi-dimensional overlap detection |

## Synchronization Model

All synchronization within `simpler` uses hardware atomics in device GM:
- `__atomic_compare_exchange_n` (CAS) for fanout locks
- `__atomic_fetch_add` for refcount updates
- `STORE_RELEASE` / `LOAD_ACQUIRE` for ring pointer visibility

This is fundamentally different from Linqu's mutex-based, message-driven model.
