#ifndef FUSELINK_DEBUG_H
#define FUSELINK_DEBUG_H

#include <cstdio>
#include <cuda.h>

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

#endif // FUSELINK_DEBUG_H