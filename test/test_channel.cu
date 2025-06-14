#include "../src/channel.h"
#include <cuda_runtime.h>
#include <iostream>
#include <vector>
#include <cuda/atomic>
#include <chrono>
#include <thread>
#include <cuda.h>
#include <atomic>
#include "task.h"

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

// Global channel arrays
// SendChannel* global_send_channels[MAX_PEERS][N_CHANNELS] = {nullptr};
// RecvChannel* global_recv_channels[MAX_PEERS][N_CHANNELS] = {nullptr};

// GPU kernel to fill task FIFO
__global__ void fillTaskFifo(CpuTaskFifo* fifo, void* data, size_t data_size) {
    int idx = blockIdx.x * blockDim.x + threadIdx.x;
    if (idx >= TASK_FIFO_SZ) return;

    // get empty slot
    if (fifo->fifo_flags[idx] == -1) {
        fifo->tasks[idx].buffer = data;
        fifo->tasks[idx].buffer_size = data_size;
        fifo->fifo_flags[idx] = data_size;
        printf("fillTaskFifo: thread %d filled slot %d with size %ld\n", 
               idx, idx, data_size);
    } else {
        printf("fillTaskFifo: thread %d couldn't find empty slot\n", idx);
    }
}

// CPU thread function to process tasks
void processTasks(SendChannel* channel) {
    channel->threadMain();
}

// Test function
void testChannelTaskPassing() {
    const int TEST_DATA_SIZE = 1024;  // 1KB test data
    const int NUM_TASKS = 8;          // Number of tasks to create
    
    // Allocate test data
    void* h_data;
    void* d_data;
    CUDA_CHECK(cudaMallocHost(&h_data, TEST_DATA_SIZE));
    CUDA_CHECK(cudaMalloc(&d_data, TEST_DATA_SIZE));
    
    // Initialize test data
    memset(h_data, 0xAA, TEST_DATA_SIZE);
    CUDA_CHECK(cudaMemcpy(d_data, h_data, TEST_DATA_SIZE, cudaMemcpyHostToDevice));
    
    // Create and initialize channel
    SendChannel* channel = new SendChannel();
    
    // Initialize size_fifo to -1 (empty)
    for (int i = 0; i < TASK_FIFO_SZ; i++) {
        channel->getTaskFifo()->fifo_flags[i] = -1;
    }
    
    // Start CPU processing thread
    std::thread cpu_thread(processTasks, channel);
    
    // Launch GPU kernel to fill task FIFO
    dim3 block(32);
    dim3 grid((NUM_TASKS + block.x - 1) / block.x);
    
    std::cout << "Launching kernel with grid size " << grid.x << " and block size " << block.x << std::endl;
    
    fillTaskFifo<<<grid, block>>>(channel->getTaskFifo(), d_data, TEST_DATA_SIZE);
    
    // Check for kernel launch errors
    CUDA_CHECK(cudaGetLastError());
    
    // Wait for GPU to finish and flush printf buffer
    CUDA_CHECK(cudaDeviceSynchronize());
    
    // Print FIFO status
    printf("GPU kernel finished\n");
    printf("FIFO status:\n");
    for (int i = 0; i < TASK_FIFO_SZ; i++) {
        printf("Slot %d: size = %d\n", i, 
               channel->getTaskFifo()->fifo_flags[i]);
    }
    
    // Wait for CPU thread to process all tasks
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    // Cleanup
    cpu_thread.join();
    CUDA_CHECK(cudaFreeHost(h_data));
    CUDA_CHECK(cudaFree(d_data));
    delete channel;
    
    std::cout << "Test completed successfully!" << std::endl;
}

int main() {
    // Enable CUDA printf
    cudaDeviceSetLimit(cudaLimitPrintfFifoSize, 1024 * 1024);
    testChannelTaskPassing();
    return 0;
} 