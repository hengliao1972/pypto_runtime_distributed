# Linqu Distributed Runtime

Linqu is a hierarchical distributed runtime for Levels 2–6 of the PyPTO machine hierarchy. It extends the `simpler` runtime (which handles Levels 0–2: Core, Chip Die, Chip) upward through Host (L3), Pod (L4), CLOS1/Supernode (L5), and CLOS2/Cluster (L6).

## Architecture

Linqu follows a **single-process orchestration** model: one orchestration process at level L submits tasks to many instances at level L−1. Every hierarchy level (L3, L4, L5, L6) runs its orchestration function and runtime in a dedicated, independent OS process. Cross-level communication uses IPC (Unix domain sockets in the verification environment).

```
L6 (CLOS2)  ──► 16 L5 processes
  L5 (CLOS1)  ──► 4 L4 processes
    L4 (POD)    ──► 16 L3 processes
      L3 (HOST)   ──► simpler runtime (L0–L2)
```

### Unified Runtime API

All levels share the same `LinquRuntimeOps` function-pointer table, mirroring `simpler`'s `PTO2RuntimeOps`. Every `.so` kernel uses the same API regardless of which level it runs at:

- `submit_task` — dispatch a kernel to a target node
- `scope_begin` / `scope_end` — RAII scope management
- `alloc_tensor` / `free_tensor` — ring-buffer memory management
- `query_peers` — discover sibling nodes at a given level
- `wait_all` — barrier synchronization

## Relationship to `simpler`

Linqu is an **adapter** on top of simpler. It does not modify simpler's code. At the L3 boundary, `SimplerDispatcher` wraps `PTO2OrchestratorState` behind the `LinquDispatcher` interface. Upper levels (L4–L6) use `RemoteDispatcher` for cross-process task submission.

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

18 unit tests covering: core identity, ring buffers, scope/tensormap, orchestrator state, process isolation (vertical and horizontal), dispatchers, discovery, daemon, dispatch engine, topology, DAG computation, ring stress, storage, profiling, and forward compatibility.

### E2E Tests

```bash
# Single daemon lifecycle
tests/e2e/test_single_daemon.sh

# 16-host pod
tests/e2e/test_single_pod.sh

# 64-host supernode (4 pods × 16 hosts)
tests/e2e/test_single_supernode.sh

# IPC roundtrip validation
tests/e2e/test_ipc_roundtrip.sh

# Orchestrator discover + shutdown
tests/e2e/test_orchestrator.sh

# Node failure resilience
tests/e2e/test_node_failure.sh
```

### 1024-Node Virtual Cluster

```bash
# Create directory topology
./venv/create_topology.sh --verify

# Launch 1024 daemon processes
./venv/launch_cluster.sh

# Check cluster health
./venv/cluster_status.sh

# Orchestrator operations
./build/linqu_orchestrator --command discover
./build/linqu_orchestrator --command shutdown

# Clean shutdown
./venv/shutdown_cluster.sh
```

## Verification Environment

The verification environment runs multiple daemon processes on a single arm64 machine. Each level runs in a separate OS process, communicating via Unix domain sockets. Storage is filesystem-based, with each node having its own directory:

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
Each L3 daemon writes `identity.json` with its coordinate and PID, verifying the hierarchical structure is correct.

### 2. Distributed DAG Test
Computes `f[i] = (a[i]+b[i]+1)(a[i]+b[i]+2) + (a[i]+b[i])` across distributed nodes using `LinquTensorMap` for cross-node data dependencies.

### 3. Ring Stress Test
Exercises ring buffers with nested scopes, early `pl.free()`, and controlled allocation pressure to verify memory management correctness.

## Project Structure

```
src/
├── core/           # Level enum, LinquCoordinate, TaskKey
├── ring/           # LinquHeapRing, LinquTaskRing, LinquDepListPool
├── runtime/        # LinquOrchestratorState, dispatchers, API header
├── transport/      # LinquHeader, msg_types, UnixSocketTransport
├── discovery/      # PeerRegistry, FilesystemDiscovery
├── daemon/         # NodeDaemon, CodeCache, Storage, main executables
└── profiling/      # RingMetrics, ProfileReport
tests/
├── unit/           # 18 unit tests
└── e2e/            # 6 end-to-end shell tests
examples/
├── topology_test/  # L3 identity kernel
├── tensor_dag_test/# Distributed DAG kernels
└── ring_stress_test/# Ring buffer stress kernels
venv/               # 1024-node cluster launch scripts
docs/               # Implementation plan
```

## Design References

- [Linqu Runtime Design](../pypto_top_level_design_documents/linqu_runtime_design.md)
- [Machine Hierarchy](../pypto_top_level_design_documents/machine_hierarchy_and_function_hierarchy.md)
