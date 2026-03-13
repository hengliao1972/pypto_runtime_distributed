# ChipBackend Adapter Specification

## Status: Design Only (Phase 0)

This document specifies the interface for the future `ChipBackend` adapter that will bridge the Tier 2 communication boundary between Linqu's host memory (L3) and `simpler`'s device GM (L0–L2). The adapter is NOT implemented in Phase 0; the `LocalDispatcher` uses a stub instead.

## Architecture

```
L4+ (Tier 3: message passing)
  └──► L3 NodeDaemon (host CPU, host memory)
         └──► ChipBackend adapter (Tier 2 bridge, within pypto_runtime_distributed)
                └──► simpler runtime (device GM, Tier 1)
                       └──► L0-L2 workers (cores, chip die, chip)
```

## Interface

```cpp
class ChipBackend {
public:
    // Initialize: dlopen simpler's libhost_runtime.so
    bool init(const std::string& simpler_lib_path);

    // Transfer input tensor from host memory to device GM
    bool h2d_copy(uint64_t linqu_handle, void* host_addr, size_t size,
                  uint64_t* device_gm_addr_out);

    // Transfer output tensor from device GM to host memory
    bool d2h_copy(uint64_t device_gm_addr, void* host_addr_out, size_t size);

    // Submit a chip-level task through simpler's API
    int submit_chip_task(const std::string& kernel_so,
                         uint64_t* device_gm_handles, int num_handles);

    // Wait for chip-level task completion
    bool wait_chip_task(int task_id);

    // Release device GM buffers for a retired scope
    void release_scope_buffers(uint16_t scope_depth);

    void shutdown();
};
```

## Key Responsibilities

### 1. Dynamic Linking (No Compile-Time Dependency)
The adapter loads `simpler`'s `libhost_runtime.so` via `dlopen`/`dlsym` at runtime. This ensures `pypto_runtime_distributed` has zero compile-time dependency on `simpler`.

### 2. Handle-to-Address Mapping
Linqu uses opaque `uint64_t` handles for tensors. The adapter maintains a `std::unordered_map<uint64_t, uint64_t>` mapping Linqu handles to device GM addresses.

### 3. Host↔Device Data Transfer
- **h2d_copy**: Before submitting a chip-level task, copy input tensor data from host DRAM to device GM. Allocate device GM buffer, perform DMA, store mapping.
- **d2h_copy**: After chip-level task completion, copy output tensor data from device GM back to host DRAM.

### 4. Buffer Lifecycle Management
Device GM buffers are allocated on `h2d_copy` and released when the L3 scope retires the corresponding ring slot. The adapter tracks per-scope buffer lists.

### 5. No Modification to `simpler`
The adapter calls `simpler`'s existing public API through a stable ABI. It does NOT modify `simpler` source code.

## Constraints

- The adapter lives entirely within `pypto_runtime_distributed`
- Phase 0: `LocalDispatcher` stubs L3→L2 dispatch (simulates completion)
- Phase 1: `ChipBackend` implements actual device dispatch when chip hardware is available
