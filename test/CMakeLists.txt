cmake_minimum_required(VERSION 3.8)
project(fuselink_test LANGUAGES CUDA CXX)

# Set CUDA specific flags
# set(CMAKE_CUDA_STANDARD 14)
set(CMAKE_CXX_STANDARD 14)
find_package(Threads REQUIRED)


# # Find CUDA
# find_package(CUDA REQUIRED)
# include_directories(${CUDA_INCLUDE_DIRS})
# Add test executables
# add_executable(test_channel 
#     test_channel.cu
#     ../src/channel.cc
# )

add_executable(test_sendrecv
    test_sendrecv.cu
    ../src/channel.cc
    ../device/device.cu
)

add_executable(test_sendrecv_general
    test_sendrecv_general.cu
    ../src/channel.cc
    ../device/device.cu
)

# Set CUDA specific flags for all test executables
set_target_properties(test_sendrecv test_sendrecv_general PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_ARCHITECTURES "70"  # Adjust based on your GPU architecture
)

# Include directories for all test executables
# target_include_directories(test_channel PRIVATE
#     ${CMAKE_SOURCE_DIR}/../src
#     ${CMAKE_SOURCE_DIR}/../src/device
# )

# Include directories for test_sendrecv
target_include_directories(test_sendrecv PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../src/device
)

target_include_directories(test_sendrecv_general PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../src/device
)

# Link pthread for both executables
# target_link_libraries(test_channel ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(test_sendrecv ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(test_sendrecv cuda)

target_link_libraries(test_sendrecv_general ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(test_sendrecv_general cuda)