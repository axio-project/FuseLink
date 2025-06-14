#include "../src/sendrecv.cu"
#include "../src/channel.h"
#include <cuda_runtime.h>
#include <iostream>
#include <vector>
#include <cuda/atomic>
#include <chrono>
#include <thread>
#include <cuda.h>
#include <atomic>

// Helper function to check CUDA errors
#define CUDA_CHECK(call) \
    do { \
        cudaError_t error = call; \
        if (error != cudaSuccess) { \
            std::cerr << "CUDA error at " << __FILE__ << ":" << __LINE__ << ": " \
                      << cudaGetErrorString(error) << std::endl; \
            exit(1); \
        } \
    } while(0)

// Test data size
const size_t TEST_DATA_SIZE = 1024 * 1024;  // 1MB test data

// Initialize test data on GPU
__global__ void initTestData(uint64_t* data, size_t size) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx < size / sizeof(uint64_t)) {
        data[idx] = idx;  // Fill with index values
    }
}

// Verify data on CPU
bool verifyData(uint64_t* data, size_t size) {
    for (size_t i = 0; i < size / sizeof(uint64_t); i++) {
        if (data[i] != i) {
            std::cerr << "Data mismatch at index " << i 
                      << ": expected " << i << ", got " << data[i] << std::endl;
            return false;
        }
    }
    return true;
}

void testFuselinkSend() {
    std::cout << "Starting fuselink_send test..." << std::endl;

    // Allocate test data
    uint64_t* d_data;
    uint64_t* h_data;
    CUDA_CHECK(cudaMalloc(&d_data, TEST_DATA_SIZE));
    CUDA_CHECK(cudaMallocHost(&h_data, TEST_DATA_SIZE));

    // Initialize test data on GPU
    // dim3 block(256);
    // dim3 grid((TEST_DATA_SIZE / sizeof(uint64_t) + block.x - 1) / block.x);
    // initTestData<<<grid, block>>>(d_data, TEST_DATA_SIZE);
    CUDA_CHECK(cudaGetLastError());
    CUDA_CHECK(cudaDeviceSynchronize());

    // Create CUDA stream
    cudaStream_t stream;
    CUDA_CHECK(cudaStreamCreate(&stream));

    // Test sending data
    std::cout << "Sending " << TEST_DATA_SIZE << " bytes..." << std::endl;
    int result = fuselink_send(d_data, TEST_DATA_SIZE, stream, 0);
    if (result != 0) {
        std::cerr << "fuselink_send failed with error code: " << result << std::endl;
        goto cleanup;
    }

    // Wait for send to complete
    CUDA_CHECK(cudaStreamSynchronize(stream));

    // Verify data was sent correctly
    // Note: In a real test, you would need to implement the receive side
    // and verify the data there. For now, we'll just check if the send completed.
    std::cout << "Send completed successfully" << std::endl;

cleanup:
    // Cleanup
    CUDA_CHECK(cudaStreamDestroy(stream));
    CUDA_CHECK(cudaFree(d_data));
    CUDA_CHECK(cudaFreeHost(h_data));
}

int main() {
    // Enable CUDA printf
    // cudaDeviceSetLimit(cudaLimitPrintfFifoSize, 1024 * 1024);
    
    // Run test
    testFuselinkSend();
    
    std::cout << "Test completed!" << std::endl;
    return 0;
}