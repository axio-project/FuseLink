#ifndef FUSELINK_SENDRECV_H
#define FUSELINK_SENDRECV_H

#include "device.h"
#include "channel.h"
#include <cstdlib>
#include <cuda_runtime.h>
#include "task.h"

#define FUSELINK_CHUNK_SZ (1 << 21) // 2MB
#define CEIL_DIV(a, b) (((a) + (b) - 1) / (b))
#define FLOOR_DIV(a, b) ((a) / (b))

int fuselink_send(void* buffer, size_t size, cudaStream_t stream, int peer_id);
int fuselink_send_general(void* buffer, size_t size, cudaStream_t stream, int peer_id, bool verify=false);

__global__ void fuselink_send_kernel(void *buffer, size_t size, GpuMem *mem, NicRing **ring, GpuTaskFifo *fifos);

__global__ void fuselink_send_general_kernel(void *buffer, size_t size, GpuMem *mem, NicRing **ring, GeneralTaskFifo *fifos);

int fuselink_recv(void* buffer, size_t size, cudaStream_t stream);

#endif