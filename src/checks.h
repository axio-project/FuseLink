#ifndef CHECKS_H_
#define CHECKS_H_

#include <cuda.h>
#include <stdio.h>

#define FUSELINK_WARN(fmt, ...) fprintf(stderr, fmt, ##__VA_ARGS__)
// Check CUDA RT calls
#define CUDACHECK(cmd) do {                                 \
    cudaError_t err = cmd;                                  \
    if( err != cudaSuccess ) {                              \
        FUSELINK_WARN("Cuda failure '%s'", cudaGetErrorString(err)); \
        exit(1);                      \
    }                                                       \
} while(false)
// check driver API calls
#define CUCHECK(cmd) do {                                 \
    CUresult err = cmd;                                  \
    if( err != CUDA_SUCCESS ) {                              \
        FUSELINK_WARN("Cuda failure '%d'", err); \
        exit(1);                      \
    }                                                       \
} while(false)

#define CUDACHECKGOTO(cmd, RES, label) do {                 \
    cudaError_t err = cmd;                                  \
    if( err != cudaSuccess ) {                              \
        FUSELINK_WARN("Cuda failure '%s'", cudaGetErrorString(err)); \
        RES = 1;                       \
        goto label;                                         \
    }                                                       \
} while(false)

#define CUCHECKGOTO(cmd, RES, label) do {                 \
    CUresult err = cmd;                                  \
    if( err != CUDA_SUCCESS ) {                              \
        FUSELINK_WARN("Cuda failure '%d'", err); \
        RES = 1;                       \
        goto label;                                         \
    }                                                       \
} while(false)

// error when call return non-zero
#define SYS_CHECK_NZ(cmd) do { \
    if (cmd != 0) { \
        FUSELINK_WARN("System call failed with error code %d", cmd); \
        exit(1); \
    } \
} while(false)

// error when call return zero
#define SYS_CHECK_Z(cmd) do { \
    if (cmd == 0) { \
        FUSELINK_WARN("System call failed with error code %d", cmd); \
        exit(1); \
    } \
} while(false)

#endif


