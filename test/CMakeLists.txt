cmake_minimum_required(VERSION 3.8)
project(fuselink_test LANGUAGES CUDA CXX)

set(CMAKE_CXX_STANDARD 14)
find_package(Threads REQUIRED)
find_package(CUDA REQUIRED)

# Include directories for all tests
include_directories(
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../src/device
)

add_executable(test_channel 
    test_channel.cu
    ../src/channel.cc
)

add_executable(test_sendrecv
    test_sendrecv.cu
    ../src/channel.cc
    ../src/device/device.cu
)

add_executable(test_channel_connection
    test_channel_connection.cc
    ../src/channel.cc
    ../src/device/device.cu
)

add_executable(test_rdma_server_roce
    rdma_server_roce.cc
    ../src/net.cc
)

add_executable(test_rdma_client_roce
    rdma_client_roce.cc
    ../src/net.cc
)

set_target_properties(test_channel PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_ARCHITECTURES "70"
)
set_target_properties(test_sendrecv PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_ARCHITECTURES "70"
)
set_target_properties(test_channel_connection PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_ARCHITECTURES "70"
)

target_link_libraries(test_channel ${CMAKE_THREAD_LIBS_INIT} cuda)
target_link_libraries(test_sendrecv ${CMAKE_THREAD_LIBS_INIT} ibverbs rdmacm cuda)
target_link_libraries(test_channel_connection ${CMAKE_THREAD_LIBS_INIT} ibverbs rdmacm cuda)
target_link_libraries(test_rdma_server_roce ${CMAKE_THREAD_LIBS_INIT} ibverbs rdmacm)
target_link_libraries(test_rdma_client_roce ${CMAKE_THREAD_LIBS_INIT} ibverbs rdmacm)