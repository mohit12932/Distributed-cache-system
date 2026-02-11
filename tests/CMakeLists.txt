cmake_minimum_required(VERSION 3.10)
project(Tests)

enable_testing()

add_executable(test_hello ai_kv_store/tests/test_hello.cpp)
target_link_libraries(test_hello gtest gtest_main)

add_test(NAME test_hello COMMAND test_hello)