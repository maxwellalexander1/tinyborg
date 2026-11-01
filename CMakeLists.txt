cmake_minimum_required(VERSION 3.16)
project(tinyborg CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(gRPC REQUIRED)

# ---- Generate C++ code from proto/borg.proto ----
set(PROTO_FILE ${CMAKE_SOURCE_DIR}/proto/borg.proto)
set(GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_DIR})

get_target_property(grpc_cpp_plugin_location gRPC::grpc_cpp_plugin LOCATION)

add_custom_command(
  OUTPUT
    ${GENERATED_DIR}/borg.pb.cc
    ${GENERATED_DIR}/borg.pb.h
    ${GENERATED_DIR}/borg.grpc.pb.cc
    ${GENERATED_DIR}/borg.grpc.pb.h
  COMMAND protobuf::protoc
  ARGS
    --cpp_out=${GENERATED_DIR}
    --grpc_out=${GENERATED_DIR}
    --plugin=protoc-gen-grpc=${grpc_cpp_plugin_location}
    -I ${CMAKE_SOURCE_DIR}/proto
    ${PROTO_FILE}
  DEPENDS ${PROTO_FILE}
)

add_library(borg_proto
  ${GENERATED_DIR}/borg.pb.cc
  ${GENERATED_DIR}/borg.grpc.pb.cc
)
target_include_directories(borg_proto PUBLIC ${GENERATED_DIR})
target_link_libraries(borg_proto
  PUBLIC
  protobuf::libprotobuf
  gRPC::grpc++
)

# ---- Subdirectories (added as we build them out) ----
# add_subdirectory(master)
# add_subdirectory(worker)
# add_subdirectory(cli)

add_executable(master_server
  master/cluster_state.cpp
  master/scheduler.cpp
  master/master_service.cpp
  master/worker_client.cpp
  master/main.cpp
)
target_include_directories(master_server PRIVATE master)
target_link_libraries(master_server PRIVATE borg_proto)

add_executable(worker_server
  worker/job_runner.cpp
  worker/main.cpp
)
target_include_directories(worker_server PRIVATE worker)
target_link_libraries(worker_server PRIVATE borg_proto)

add_executable(tbctl
  cli/tbctl.cpp
)
target_link_libraries(tbctl PRIVATE borg_proto)

enable_testing()
add_test(NAME kill_worker COMMAND bash ${CMAKE_SOURCE_DIR}/tests/integration/kill_worker_test.sh)

include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(scheduler_test
  master/cluster_state.cpp
  master/scheduler.cpp
  tests/unit/scheduler_test.cpp
)
target_include_directories(scheduler_test PRIVATE master)
target_link_libraries(scheduler_test PRIVATE GTest::gtest_main)
add_test(NAME scheduler_test COMMAND scheduler_test)

add_executable(job_runner_gtest
  worker/job_runner.cpp
  tests/unit/job_runner_test.cpp
)
target_include_directories(job_runner_gtest PRIVATE worker)
target_link_libraries(job_runner_gtest PRIVATE GTest::gtest_main)
add_test(NAME job_runner_gtest COMMAND job_runner_gtest)
