cmake_minimum_required(VERSION 3.10)
project(KVStore)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/include)

set(SOURCES
    main.cpp
    src/CommandParser.cpp
    src/TCPServer.cpp
    src/KVStore.cpp
)

add_executable(kvstore ${SOURCES})

target_link_libraries(kvstore pthread)