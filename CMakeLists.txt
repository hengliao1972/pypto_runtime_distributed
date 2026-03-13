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

# Runtime library (orchestrator state, dispatchers)
file(GLOB RUNTIME_SRC src/runtime/*.cpp)
if(RUNTIME_SRC)
    add_library(linqu_runtime_lib STATIC ${RUNTIME_SRC})
    if(PROFILING_SRC)
        target_link_libraries(linqu_runtime_lib linqu_profiling)
    endif()
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
linqu_add_test(test_scope_tensormap tests/unit/test_scope_tensormap.cpp linqu_runtime_lib linqu_ring)

# ---- Milestone 4B: OrchestratorState integration test ----
linqu_add_test(test_orchestrator_state tests/unit/test_orchestrator_state.cpp linqu_runtime_lib linqu_ring)

# ---- Milestone 6: Transport + Process isolation test ----
if(TRANSPORT_SRC)
    linqu_add_test(test_process_isolation tests/unit/test_process_isolation.cpp linqu_transport)
    linqu_add_test(test_multilevel_chain tests/unit/test_multilevel_chain.cpp linqu_transport)
    linqu_add_test(test_fanout_isolation tests/unit/test_fanout_isolation.cpp linqu_transport)
    linqu_add_test(test_dispatchers tests/unit/test_dispatchers.cpp linqu_runtime_lib linqu_ring linqu_transport)
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
    linqu_add_test(test_storage tests/unit/test_storage.cpp linqu_daemon_lib)
    linqu_add_test(test_profiling tests/unit/test_profiling.cpp linqu_runtime_lib linqu_ring linqu_profiling)
    linqu_add_test(test_forward_compat tests/unit/test_forward_compat.cpp linqu_daemon_lib)
endif()
