cmake_minimum_required(VERSION 3.10)

# KVStore + Utils + UserBook source, shared by the offline (no-server-needed)
# unit tests.
set(KVSTORE_SOURCES
    ${CMAKE_SOURCE_DIR}/src/KVStore.cpp
    ${CMAKE_SOURCE_DIR}/src/Utils.cpp
    ${CMAKE_SOURCE_DIR}/src/UserBook.cpp
)

# -----------------------------------------------------------------------
# unit/ -- link against KVStore/Utils/UserBook directly, no live server,
# no network involved. Registered with CTest.
# -----------------------------------------------------------------------

add_executable(test_kv_store unit/test_kv_store.cpp ${KVSTORE_SOURCES})
target_include_directories(test_kv_store PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_kv_store gtest gtest_main pthread)

add_executable(test_utils unit/test_utils.cpp ${KVSTORE_SOURCES})
target_include_directories(test_utils PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_utils gtest gtest_main pthread)

add_executable(test_user_book unit/test_user_book.cpp ${KVSTORE_SOURCES})
target_include_directories(test_user_book PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_user_book gtest gtest_main pthread)

add_executable(test_persistence unit/test_persistence.cpp ${KVSTORE_SOURCES})
target_include_directories(test_persistence PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_persistence gtest gtest_main pthread)

enable_testing()
add_test(NAME KVStoreTests COMMAND test_kv_store)
add_test(NAME UtilsTests COMMAND test_utils)
add_test(NAME UserBookTests COMMAND test_user_book)
add_test(NAME PersistenceTests COMMAND test_persistence)

# -----------------------------------------------------------------------
# integration/ -- C++ client/server integration tests.
# Require a running ./kvstore server on 127.0.0.1:8080.
# Registered as build targets but executed by scripts/run/run_tests.sh
# because they require a live server.
# -----------------------------------------------------------------------

add_executable(test_full_flow
    integration/test_full_flow.cpp
    ${CMAKE_SOURCE_DIR}/src/TCPServer.cpp
    ${CMAKE_SOURCE_DIR}/src/KVStore.cpp
    ${CMAKE_SOURCE_DIR}/src/UserBook.cpp
    ${CMAKE_SOURCE_DIR}/src/Utils.cpp
)

target_include_directories(test_full_flow PRIVATE ${CMAKE_SOURCE_DIR}/include)

target_link_libraries(test_full_flow
    gtest
    gtest_main
    pthread
)

add_executable(test_server_client
    integration/test_server_client.cpp
    ${CMAKE_SOURCE_DIR}/src/TCPServer.cpp
    ${CMAKE_SOURCE_DIR}/src/KVStore.cpp
    ${CMAKE_SOURCE_DIR}/src/UserBook.cpp
    ${CMAKE_SOURCE_DIR}/src/Utils.cpp
)

target_include_directories(test_server_client
    PRIVATE ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(test_server_client
    gtest
    gtest_main
    pthread
)

# -----------------------------------------------------------------------
# stress/ and chaos/ (C++ binaries) -- pure network clients, no KVStore
# sources needed. Require a running ./kvstore server on 127.0.0.1:8080.
# Not registered with CTest -- run via scripts/run/run_tests.sh, which
# manages the server lifecycle around each one.
#
# unit/test_data_integrity.py and chaos/TCP_chaos_test.py and
# chaos/malformed_protocol.py are plain Python and are not CMake targets;
# they're invoked directly by scripts/run/run_tests.sh.
# -----------------------------------------------------------------------

add_executable(test_scalability stress/scalability_test.cpp)
target_link_libraries(test_scalability pthread)

add_executable(concurrency_stress
    stress/concurrency_stress.cpp
    ${CMAKE_SOURCE_DIR}/src/KVStore.cpp
    ${CMAKE_SOURCE_DIR}/src/UserBook.cpp
    ${CMAKE_SOURCE_DIR}/src/Utils.cpp
)

target_include_directories(concurrency_stress
    PRIVATE ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(concurrency_stress
    pthread
)

add_executable(user_stress
    stress/user_stress.cpp
    ${CMAKE_SOURCE_DIR}/src/UserBook.cpp
)

target_include_directories(user_stress
    PRIVATE ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(user_stress
    pthread
)

add_executable(disconnect_test
    chaos/disconnect_test.cpp
    ${CMAKE_SOURCE_DIR}/src/TCPServer.cpp
    ${CMAKE_SOURCE_DIR}/src/KVStore.cpp
    ${CMAKE_SOURCE_DIR}/src/UserBook.cpp
    ${CMAKE_SOURCE_DIR}/src/Utils.cpp
)

target_include_directories(disconnect_test
    PRIVATE ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(disconnect_test
    gtest
    gtest_main
    pthread
)