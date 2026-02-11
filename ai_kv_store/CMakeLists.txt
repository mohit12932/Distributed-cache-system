cmake_minimum_required(VERSION 3.16)
project(ai_kv_store VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ── Dependencies via FetchContent ──

include(FetchContent)

# Abseil
FetchContent_Declare(
    abseil-cpp
    GIT_REPOSITORY https://github.com/abseil/abseil-cpp.git
    GIT_TAG        20240116.2
)
FetchContent_MakeAvailable(abseil-cpp)

# gRPC (includes protobuf)
FetchContent_Declare(
    grpc
    GIT_REPOSITORY https://github.com/grpc/grpc.git
    GIT_TAG        v1.62.0
    GIT_SHALLOW    TRUE
)
set(FETCHCONTENT_QUIET OFF)
FetchContent_MakeAvailable(grpc)

# ── Protobuf code generation ──

set(PROTO_SRC_DIR ${CMAKE_SOURCE_DIR}/proto)
set(PROTO_GEN_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PROTO_GEN_DIR})

set(_PROTOBUF_PROTOC $<TARGET_FILE:protobuf::protoc>)
set(_GRPC_CPP_PLUGIN $<TARGET_FILE:grpc_cpp_plugin>)

set(PROTO_FILES ${PROTO_SRC_DIR}/kv_store.proto)

foreach(PROTO_FILE ${PROTO_FILES})
    get_filename_component(PROTO_NAME ${PROTO_FILE} NAME_WE)
    set(PROTO_GEN_CC  ${PROTO_GEN_DIR}/${PROTO_NAME}.pb.cc)
    set(PROTO_GEN_H   ${PROTO_GEN_DIR}/${PROTO_NAME}.pb.h)
    set(GRPC_GEN_CC   ${PROTO_GEN_DIR}/${PROTO_NAME}.grpc.pb.cc)
    set(GRPC_GEN_H    ${PROTO_GEN_DIR}/${PROTO_NAME}.grpc.pb.h)

    add_custom_command(
        OUTPUT ${PROTO_GEN_CC} ${PROTO_GEN_H} ${GRPC_GEN_CC} ${GRPC_GEN_H}
        COMMAND ${_PROTOBUF_PROTOC}
            --cpp_out=${PROTO_GEN_DIR}
            --grpc_out=${PROTO_GEN_DIR}
            --plugin=protoc-gen-grpc=${_GRPC_CPP_PLUGIN}
            -I ${PROTO_SRC_DIR}
            ${PROTO_FILE}
        DEPENDS ${PROTO_FILE}
        COMMENT "Generating protobuf/gRPC code for ${PROTO_NAME}"
    )
    list(APPEND GENERATED_SOURCES ${PROTO_GEN_CC} ${GRPC_GEN_CC})
    list(APPEND GENERATED_HEADERS ${PROTO_GEN_H} ${GRPC_GEN_H})
endforeach()

# ── Generated proto library ──

add_library(ai_kv_proto STATIC ${GENERATED_SOURCES})
target_include_directories(ai_kv_proto PUBLIC ${PROTO_GEN_DIR})
target_link_libraries(ai_kv_proto PUBLIC
    grpc++
    grpc++_reflection
    protobuf::libprotobuf
)

# ── Main node binary ──

add_executable(ai_kv_node src/main.cpp)
target_include_directories(ai_kv_node PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${PROTO_GEN_DIR}
)
target_link_libraries(ai_kv_node PRIVATE
    ai_kv_proto
    absl::flags
    absl::flags_parse
    absl::strings
    absl::str_format
    absl::hash
    absl::synchronization
    absl::time
    grpc++
)

# ── Tests ──

enable_testing()

# Storage engine tests
add_executable(test_storage tests/test_storage.cpp)
target_include_directories(test_storage PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_storage PRIVATE
    absl::strings
    absl::synchronization
)
add_test(NAME StorageTests COMMAND test_storage)

# PINN model tests
add_executable(test_pinn tests/test_pinn.cpp)
target_include_directories(test_pinn PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(test_pinn PRIVATE
    absl::strings
    absl::synchronization
)
add_test(NAME PINNTests COMMAND test_pinn)

# ── Install ──

install(TARGETS ai_kv_node RUNTIME DESTINATION bin)
install(DIRECTORY proto/ DESTINATION share/ai_kv_store/proto)
