cmake_minimum_required(VERSION 3.18)
project(fuselink LANGUAGES CUDA CXX)

# Set CUDA specific flags
set(CMAKE_CUDA_STANDARD 17)  # Required for cuda::std::atomic
set(CMAKE_CUDA_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find CUDA
find_package(CUDA REQUIRED)

# Set CUDA specific flags
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcompiler=-Wall,-Wextra")
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} --expt-relaxed-constexpr")

# Add source files
set(SOURCES
    src/sendrecv.cu
    src/channel.cc
    src/device/device.cu
)

# Add header files
set(HEADERS
    src/task.h
    src/sendrecv.cuh
    src/channel.h
    src/device/device.h
)

# Create library
add_library(fuselink STATIC ${SOURCES} ${HEADERS})

# Set include directories
target_include_directories(fuselink PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/device
)

# Set CUDA specific properties
set_target_properties(fuselink PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    CUDA_ARCHITECTURES "70"  # Adjust based on your GPU architecture
)

# Link against CUDA libraries
target_link_libraries(fuselink PUBLIC
    cuda
    cudart
    ${CMAKE_THREAD_LIBS_INIT}
)

# Add subdirectory for tests
add_subdirectory(test) 