cmake_minimum_required(VERSION 3.10)

# KVStore + Utils source, needed only by the internal (in-process) benchmark.
set(KVSTORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/KVStore.cpp
    ${CMAKE_SOURCE_DIR}/src/Utils.cpp
)

# -----------------------------------------------------------------------
# Internal micro-benchmarks -- Set/Get/Del directly against KVStore, no
# network involved.
# -----------------------------------------------------------------------

add_executable(bench_internal bench_internal_engine.cpp ${KVSTORE_SOURCES})
target_include_directories(bench_internal PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(bench_internal benchmark benchmark_main pthread)

# -----------------------------------------------------------------------
# Whole-server latency vs. data size -- cold (server grows from empty during
# measurement) and steady-state (each connection pre-seeded to a target
# per-connection key count before measurement). Pure clients, require a
# running ./kvstore server on 127.0.0.1:8080.
# -----------------------------------------------------------------------

add_executable(cold_server_latency cold_server_latency.cpp)
target_link_libraries(cold_server_latency pthread)

add_executable(steady_server_latency steady_server_latency.cpp)
target_link_libraries(steady_server_latency pthread)

# -----------------------------------------------------------------------
# Network benchmarks -- pure TCP clients, require a live ./kvstore server on
# 127.0.0.1:8080. Built by CI but not executed there; run manually to
# characterize throughput (bench_throughput) and long-term stability
# (bench_endurance) under sustained load.
# -----------------------------------------------------------------------

add_executable(bench_throughput bench_net_throughput.cpp)
target_link_libraries(bench_throughput pthread)

add_executable(bench_endurance bench_net_endurance.cpp)
target_link_libraries(bench_endurance pthread)