# Linqu Distributed Runtime

Linqu is a self-contained hierarchical distributed runtime for Levels 3–6 of the PyPTO machine hierarchy. It does NOT link against or modify the `simpler` runtime (which handles Levels 0–2). Integration between Linqu (L3+) and `simpler` (L0–L2) will be achieved via a future `ChipBackend` adapter; in Phase 0, L3→L2 dispatch is stubbed.

## Architecture

Linqu follows a **single-process orchestration** model: one orchestration process at level L submits tasks to many instances at level L−1. Every hierarchy level (L3, L4, L5, L6) runs its orchestration function and runtime in a dedicated, independent OS process. Cross-level communication uses IPC (Unix domain sockets in the verification environment).

```
L6 (CLOS2)  ──► 16 L5 processes
  L5 (CLOS1)  ──► 4 L4 processes
    L4 (POD)    ──► 16 L3 processes
      L3 (HOST)   ──► stub dispatch to L2 (future: ChipBackend → simpler)
```

### Three-Tier Communication Model

```
Tier 3: Message Passing (L4–L6 ↔ L3) — Unix Socket / TCP / RDMA
Tier 2: Host-Device DMA (L3 ↔ L2)    — h2d_copy / d2h_copy (future)
Tier 1: Shared Device GM (L0–L2)      — atomics + barriers (simpler, not linked)
```

### Unified Runtime API

All levels share the same `LinquRuntimeOps` function-pointer table. Every `.so` kernel uses the same API regardless of which level it runs at:

- `submit_task` — dispatch a kernel to a target node
- `scope_begin` / `scope_end` — RAII scope management
- `alloc_tensor` / `free_tensor` — ring-buffer memory management
- `query_peers` — discover sibling nodes at a given level
- `wait_all` — barrier synchronization

## Relationship to `simpler`

`simpler` is a **separate, immutable codebase** (`pypto_workspace/simpler/`) that handles Levels 0–2. This project (`pypto_runtime_distributed`) does NOT link against `simpler`, does NOT include `simpler` headers, and does NOT call any `simpler` API. The two runtimes are completely independent. A future `ChipBackend` adapter (within this project) will bridge the L3↔L2 boundary via dynamic linking (`dlopen`) and `h2d_copy`/`d2h_copy` DMA — without modifying `simpler`.

## Building

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Debug
make -j$(nproc)
```

Produces:
- `linqu_daemon` — per-node daemon executable
- `linqu_orchestrator` — top-level cluster orchestrator

Requires: C++17, aarch64 (arm64), CMake ≥ 3.16.

## Running Tests

### Unit Tests

```bash
cd build && ctest --output-on-failure -j4
```

25+ unit tests covering: core identity, ring buffers, scope/tensormap, orchestrator state, process isolation, dispatchers, discovery, daemon, dispatch engine, topology, DAG computation, ring stress, storage, profiling, forward compatibility, completion wiring, scheduler, ring retirement, back-pressure, query peers, param passing, and enhanced tensormap.

### E2E Tests

```bash
tests/e2e/test_single_daemon.sh      # Single daemon lifecycle
tests/e2e/test_single_pod.sh         # 16-host pod
tests/e2e/test_single_supernode.sh   # 64-host supernode
tests/e2e/test_ipc_roundtrip.sh      # IPC roundtrip validation
tests/e2e/test_orchestrator.sh       # Orchestrator discover + shutdown
tests/e2e/test_node_failure.sh       # Node failure resilience
```

### 1024-Node Virtual Cluster

```bash
./venv/create_topology.sh --verify
./venv/launch_cluster.sh
./venv/cluster_status.sh
./build/linqu_orchestrator --command discover
./venv/shutdown_cluster.sh
```

## Verification Environment

Each hierarchy node runs as a separate OS process on the same arm64 machine:

```
/tmp/linqu/<cluster>/L6_0/L5_<s>/L4_<p>/L3_<h>/
├── daemon_L3.sock     # Unix domain socket
├── code_cache/        # Received .so kernels
├── data_cache/        # Data buffers
└── logs/
    ├── daemon.log
    └── ring_profile.json
```

## Test Programs

### 1. Topology Test
Each level dispatches to the level below. L3 daemons write `identity.json` with coordinates and PID. Verification checks hierarchical structure correctness across all 1024 nodes.

### 2. Distributed DAG Test
Computes `f[i] = (a[i]+b[i]+1)(a[i]+b[i]+2) + (a[i]+b[i])` across distributed nodes using `LinquTensorMap` for cross-node data dependencies.

### 3. Ring Stress Test
Exercises ring buffers with nested scopes, early `pl.free()`, and controlled allocation pressure to verify memory management correctness.

## Project Structure

```
src/
├── core/           # Level enum, LinquCoordinate, TaskKey, NodeIdentity
├── ring/           # LinquHeapRing, LinquTaskRing, LinquDepListPool
├── runtime/        # LinquOrchestratorState, dispatchers, API header
├── transport/      # LinquHeader, msg_types, UnixSocketTransport
├── discovery/      # PeerRegistry, FilesystemDiscovery
├── daemon/         # NodeDaemon, CodeCache, DataCache, Storage
└── profiling/      # RingMetrics, ProfileReport
tests/
├── unit/           # 25+ unit tests
├── forward_compat/ # Mock 7-level topology tests
└── e2e/            # End-to-end shell tests
examples/
├── topology_test/  # L3-L6 topology kernels + verification
├── tensor_dag_test/# L0-L6 distributed DAG kernels + verification
└── ring_stress_test/# Ring buffer stress kernels + verification
venv/               # 1024-node cluster launch scripts
docs/               # Implementation plan, API contract, adapter spec
```

## Design References

- [Linqu Runtime Design](../pypto_top_level_design_documents/linqu_runtime_design.md)
