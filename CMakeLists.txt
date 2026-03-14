cmake_minimum_required(VERSION 3.16)
project(linqu_runtime LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# All headers findable via #include "core/level.h" etc.
include_directories(${CMAKE_SOURCE_DIR}/src)

# Warnings
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

# ---- Libraries (built incrementally as milestones are completed) ----

# Core identity library
file(GLOB CORE_SRC src/core/*.cpp)
if(CORE_SRC)
    add_library(linqu_core STATIC ${CORE_SRC})
endif()

# Ring buffer library
file(GLOB RING_SRC src/ring/*.cpp)
if(RING_SRC)
    add_library(linqu_ring STATIC ${RING_SRC})
endif()

# Profiling library
file(GLOB PROFILING_SRC src/profiling/*.cpp)
if(PROFILING_SRC)
    add_library(linqu_profiling STATIC ${PROFILING_SRC})
endif()

# Transport library
file(GLOB TRANSPORT_SRC src/transport/*.cpp)
if(TRANSPORT_SRC)
    add_library(linqu_transport STATIC ${TRANSPORT_SRC})
endif()

# Discovery library
file(GLOB DISCOVERY_SRC src/discovery/*.cpp)
if(DISCOVERY_SRC)
    add_library(linqu_discovery STATIC ${DISCOVERY_SRC})
endif()

# Runtime library (orchestrator state, dispatchers)
file(GLOB RUNTIME_SRC src/runtime/*.cpp)
if(RUNTIME_SRC)
    add_library(linqu_runtime_lib STATIC ${RUNTIME_SRC})
    if(PROFILING_SRC)
        target_link_libraries(linqu_runtime_lib linqu_profiling)
    endif()
    if(DISCOVERY_SRC AND TRANSPORT_SRC)
        target_link_libraries(linqu_runtime_lib linqu_discovery linqu_transport)
    endif()
    target_link_libraries(linqu_runtime_lib linqu_ring linqu_core dl pthread)
endif()

# Daemon library (exclude main.cpp and orchestrator_main.cpp from the library)
file(GLOB DAEMON_SRC src/daemon/*.cpp)
list(FILTER DAEMON_SRC EXCLUDE REGEX "main\\.cpp$")
list(FILTER DAEMON_SRC EXCLUDE REGEX "orchestrator_main\\.cpp$")
if(DAEMON_SRC)
    add_library(linqu_daemon_lib STATIC ${DAEMON_SRC})
    target_link_libraries(linqu_daemon_lib linqu_runtime_lib linqu_ring linqu_transport linqu_discovery dl pthread)
endif()

# Daemon executable
if(EXISTS ${CMAKE_SOURCE_DIR}/src/daemon/main.cpp)
    add_executable(linqu_daemon src/daemon/main.cpp)
    target_link_libraries(linqu_daemon linqu_daemon_lib)
endif()

# Orchestrator executable
if(EXISTS ${CMAKE_SOURCE_DIR}/src/daemon/orchestrator_main.cpp)
    add_executable(linqu_orchestrator src/daemon/orchestrator_main.cpp)
    target_link_libraries(linqu_orchestrator linqu_daemon_lib)
endif()

# ---- Test infrastructure ----
enable_testing()

# Simple test runner macro
macro(linqu_add_test TEST_NAME TEST_SRC)
    add_executable(${TEST_NAME} ${TEST_SRC})
    target_link_libraries(${TEST_NAME} ${ARGN})
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
endmacro()

# ---- Milestone 2: Core identity tests ----
linqu_add_test(test_core_identity tests/unit/test_core_identity.cpp)

# ---- Milestone 1B: Orchestration API tests ----
linqu_add_test(test_orchestration_api tests/unit/test_orchestration_api.cpp)

# ---- Milestone 3: Ring buffer tests ----
linqu_add_test(test_ring_buffers tests/unit/test_ring_buffers.cpp linqu_ring)

# ---- Milestone 4: Scope + TensorMap tests ----
linqu_add_test(test_scope_tensormap tests/unit/test_scope_tensormap.cpp linqu_runtime_lib linqu_ring linqu_discovery linqu_transport)

# ---- Milestone 4B: OrchestratorState integration test ----
linqu_add_test(test_orchestrator_state tests/unit/test_orchestrator_state.cpp linqu_runtime_lib linqu_ring linqu_discovery linqu_transport)

# ---- Milestone 6: Transport + Process isolation test ----
if(TRANSPORT_SRC)
    linqu_add_test(test_process_isolation tests/unit/test_process_isolation.cpp linqu_transport)
    linqu_add_test(test_multilevel_chain tests/unit/test_multilevel_chain.cpp linqu_transport)
    linqu_add_test(test_fanout_isolation tests/unit/test_fanout_isolation.cpp linqu_transport)
    linqu_add_test(test_dispatchers tests/unit/test_dispatchers.cpp linqu_runtime_lib linqu_ring linqu_transport linqu_discovery)
endif()

# ---- Milestone 7: Discovery tests ----
if(DISCOVERY_SRC AND TRANSPORT_SRC)
    linqu_add_test(test_discovery tests/unit/test_discovery.cpp linqu_discovery linqu_transport)
endif()

# ---- Milestone 8: Daemon tests ----
if(DAEMON_SRC)
    linqu_add_test(test_daemon tests/unit/test_daemon.cpp linqu_daemon_lib)
    linqu_add_test(test_dispatch_engine tests/unit/test_dispatch_engine.cpp linqu_daemon_lib)
    linqu_add_test(test_topology tests/unit/test_topology.cpp linqu_daemon_lib)
    linqu_add_test(test_dag tests/unit/test_dag.cpp linqu_daemon_lib)
    linqu_add_test(test_ring_stress tests/unit/test_ring_stress.cpp linqu_daemon_lib)
    linqu_add_test(test_dfs_sum_hierarchy tests/unit/test_dfs_sum_hierarchy.cpp linqu_runtime_lib linqu_ring linqu_core pthread)
    linqu_add_test(test_dfs_sum_multiprocess tests/unit/test_dfs_sum_multiprocess.cpp linqu_daemon_lib)
    linqu_add_test(test_storage tests/unit/test_storage.cpp linqu_daemon_lib)
    linqu_add_test(test_profiling tests/unit/test_profiling.cpp linqu_runtime_lib linqu_ring linqu_profiling linqu_discovery linqu_transport)
    linqu_add_test(test_forward_compat tests/unit/test_forward_compat.cpp linqu_daemon_lib)
    linqu_add_test(test_completion_wiring tests/unit/test_completion_wiring.cpp linqu_daemon_lib)
    linqu_add_test(test_scheduler tests/unit/test_scheduler.cpp linqu_daemon_lib)
    linqu_add_test(test_ring_retirement tests/unit/test_ring_retirement.cpp linqu_daemon_lib)
    linqu_add_test(test_backpressure tests/unit/test_backpressure.cpp linqu_daemon_lib)
    linqu_add_test(test_query_peers tests/unit/test_query_peers.cpp linqu_daemon_lib)
    linqu_add_test(test_param_passing tests/unit/test_param_passing.cpp linqu_daemon_lib)
    linqu_add_test(test_tensormap_enhanced tests/unit/test_tensormap_enhanced.cpp linqu_runtime_lib linqu_ring linqu_discovery linqu_transport)
endif()

# ---- Forward Compatibility tests ----
if(DISCOVERY_SRC AND TRANSPORT_SRC)
    linqu_add_test(test_peer_registry_multi_level tests/forward_compat/test_peer_registry_multi_level.cpp linqu_discovery linqu_transport)
    linqu_add_test(test_7_level_smoke tests/forward_compat/test_7_level_smoke.cpp linqu_transport)
endif()

# ---- Kernel shared libraries (for verification tests) ----
set(KERNEL_LINK_FLAGS "-shared -fPIC")

macro(linqu_add_kernel KERNEL_NAME KERNEL_SRC)
    add_library(${KERNEL_NAME} SHARED ${KERNEL_SRC})
    set_target_properties(${KERNEL_NAME} PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        POSITION_INDEPENDENT_CODE ON
    )
endmacro()

# Topology kernels
linqu_add_kernel(topo_L3_host examples/topology_test/kernels/topo_L3_host.cpp)
linqu_add_kernel(topo_L4_pod examples/topology_test/kernels/topo_L4_pod.cpp)
linqu_add_kernel(topo_L5_clos1 examples/topology_test/kernels/topo_L5_clos1.cpp)
linqu_add_kernel(topo_L6_clos2 examples/topology_test/kernels/topo_L6_clos2.cpp)

# DAG kernels
linqu_add_kernel(dag_L3_compute examples/tensor_dag_test/kernels/dag_L3_compute.cpp)
linqu_add_kernel(dag_L4_dispatch examples/tensor_dag_test/kernels/dag_L4_dispatch.cpp)
linqu_add_kernel(dag_L5_dispatch examples/tensor_dag_test/kernels/dag_L5_dispatch.cpp)
linqu_add_kernel(dag_L6_dispatch examples/tensor_dag_test/kernels/dag_L6_dispatch.cpp)

# Ring stress kernels
linqu_add_kernel(ring_L3_stress examples/ring_stress_test/kernels/ring_L3_stress.cpp)
linqu_add_kernel(ring_L4_stress examples/ring_stress_test/kernels/ring_L4_stress.cpp)
linqu_add_kernel(ring_L5_stress examples/ring_stress_test/kernels/ring_L5_stress.cpp)

# DFS sum kernels
linqu_add_kernel(dfs_L3_reader examples/dfs_sum_test/kernels/dfs_L3_reader.cpp)
