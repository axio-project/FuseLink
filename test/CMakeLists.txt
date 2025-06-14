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
add_executable(test_channel 
    test_channel.cu
    ../src/channel.cc
)

add_executable(test_sendrecv
    test_sendrecv.cu
    ../src/channel.cc
    ../device/device.cu
)

# Link against CUDA libraries
# target_link_libraries(test_channel ${CUDA_LIBRARIES})

# Set CUDA specific flags for test_channel
set_target_properties(test_channel PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_ARCHITECTURES "70"  # Adjust based on your GPU architecture
)

# Set CUDA specific flags for test_sendrecv
set_target_properties(test_sendrecv PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_ARCHITECTURES "70"  # Adjust based on your GPU architecture
)

# Include directories for test_channel
target_include_directories(test_channel PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../src/device
)

# Include directories for test_sendrecv
target_include_directories(test_sendrecv PRIVATE
    ${CMAKE_SOURCE_DIR}/../src
    ${CMAKE_SOURCE_DIR}/../src/device
)

# Link pthread for both executables
target_link_libraries(test_channel ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(test_sendrecv ${CMAKE_THREAD_LIBS_INIT})
target_link_libraries(test_sendrecv cuda)