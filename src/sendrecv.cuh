#ifndef FUSELINK_SENDRECV_H
#define FUSELINK_SENDRECV_H

#include "device.h"
#include "channel.h"
#include <cstdlib>
#include <cuda_runtime.h>
#include "task.h"

#define FUSELINK_CHUNK_SZ (1 << 19) // 512KB
#define CEIL_DIV(a, b) (((a) + (b) - 1) / (b))
#define FLOOR_DIV(a, b) ((a) / (b))

int fuselink_send(void* buffer, size_t size, cudaStream_t stream, int peer_id);

__global__ void fuselink_send_kernel(void *buffer, size_t size, GpuMem *mem, NicRing **ring, GpuTaskFifo *fifos);

int fuselink_recv(void* buffer, size_t size, cudaStream_t stream);

#endif