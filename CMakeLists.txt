cmake_minimum_required(VERSION 3.16)
project(distributed_cache VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ── Compiler warnings ──────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ── Main executable ────────────────────────────────────────────────────
add_executable(distributed_cache src/main.cpp)

target_include_directories(distributed_cache PRIVATE ${CMAKE_SOURCE_DIR})

# Link Winsock on Windows
if(WIN32)
    target_link_libraries(distributed_cache PRIVATE ws2_32)
endif()

# Pthreads on Linux/Mac
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(distributed_cache PRIVATE Threads::Threads)
endif()

# ── Test executable ────────────────────────────────────────────────────
enable_testing()

add_executable(cache_tests src/tests/test_lru_cache.cpp)
target_include_directories(cache_tests PRIVATE ${CMAKE_SOURCE_DIR})
if(WIN32)
    target_link_libraries(cache_tests PRIVATE ws2_32)
endif()
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(cache_tests PRIVATE Threads::Threads)
endif()

add_test(NAME CacheTests COMMAND cache_tests)

add_executable(concurrency_tests src/tests/test_concurrency.cpp)
target_include_directories(concurrency_tests PRIVATE ${CMAKE_SOURCE_DIR})
if(WIN32)
    target_link_libraries(concurrency_tests PRIVATE ws2_32)
endif()
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(concurrency_tests PRIVATE Threads::Threads)
endif()

add_test(NAME ConcurrencyTests COMMAND concurrency_tests)

add_executable(resp_tests src/tests/test_resp_parser.cpp)
target_include_directories(resp_tests PRIVATE ${CMAKE_SOURCE_DIR})
if(WIN32)
    target_link_libraries(resp_tests PRIVATE ws2_32)
endif()
if(UNIX)
    find_package(Threads REQUIRED)
    target_link_libraries(resp_tests PRIVATE Threads::Threads)
endif()

add_test(NAME RespTests COMMAND resp_tests)
