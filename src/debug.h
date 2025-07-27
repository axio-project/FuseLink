#ifndef FUSELINK_DEBUG_H
#define FUSELINK_DEBUG_H

#include <cstdio>
#include <cuda.h>
#include <chrono>

#define DEBUG_PRINT(fmt, ...) \
  do { \
    printf("[DEBUG] %s:%d: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
  } while (0)

#define CUDA_CHECK(call) do { \
    cudaError_t err = call; \
    if (err != cudaSuccess) { \
        printf("CUDA error %d: %s on line %d in file %s\n", err, cudaGetErrorString(err), __LINE__, __FILE__); \
        exit(1); \
    } \
} while(0)

#define CUCHECK(call) do { \
    CUresult err = call; \
    if (err != CUDA_SUCCESS) { \
        printf("CUDA error %d on line %d in file %s\n", err, __LINE__, __FILE__); \
        exit(1); \
    } \
} while(0)

class SimulateTask {
  public:
    SimulateTask(int ring_id, int slot) {
      start_time = std::chrono::high_resolution_clock::now();
      this->ring_id = ring_id;
      this->slot = slot;
    }
    SimulateTask(int ring_id, int slot, int chunk_id) {
      start_time = std::chrono::high_resolution_clock::now();
      this->ring_id = ring_id;
      this->slot = slot;
      this->chunk_id = chunk_id;
    }
  
    bool finished() {
      return true;
      // return std::chrono::high_resolution_clock::now() - start_time > std::chrono::milliseconds(1);
    }
  
    int ring_id;
    std::chrono::time_point<std::chrono::high_resolution_clock> start_time;
    int slot;
    int chunk_id;
};

#endif // FUSELINK_DEBUG_H