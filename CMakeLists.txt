cmake_minimum_required(VERSION 3.10)
project(KVStore)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_SOURCE_DIR}/include)

# -O3 for every target in this project and its subdirectories (tests/,
# benchmarks/), skipped automatically when a sanitizer build is in progress
# -- detected via -fsanitize already present in CMAKE_CXX_FLAGS. This does
# NOT touch CMAKE_BUILD_TYPE, so it can't collide with whatever build type
# (if any) build_asan.sh/build_tsan.sh already set.
if(NOT CMAKE_CXX_FLAGS MATCHES "-fsanitize")
    add_compile_options(-O3)
endif()

set(SOURCES
    main.cpp
    src/Utils.cpp
    src/TCPServer.cpp
    src/KVStore.cpp
    src/UserBook.cpp
)

add_executable(kvstore ${SOURCES})

target_link_libraries(kvstore pthread)

# Test/benchmark subprojects each own their own CMakeLists.txt and build
# their own set of executables against this project's src/ and include/.
enable_testing()
add_subdirectory(tests)
add_subdirectory(benchmarks)