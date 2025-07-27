#ifndef FUSELINK_SENDRECV_H
#define FUSELINK_SENDRECV_H

#include "device.h"
#include "channel.h"
#include <cstdlib>
#include <cuda_runtime.h>
#include "task.h"

#include "common.h"

int fuselink_send(void* buffer, size_t size, cudaStream_t stream, int peer_id);
int fuselink_send_general(void* buffer, size_t size, cudaStream_t stream, int peer_id, bool verify=false);

__global__ void fuselink_send_kernel(void *buffer, size_t size, GpuMem *mem, NicRing **ring, GpuTaskFifo *fifos);

__global__ void fuselink_send_general_kernel(void *buffer, size_t size, GpuMem *mem, NicRing **ring, GeneralTaskFifo *fifos);

__global__ void fuselink_recv_kernel(void *buffer, size_t size, GpuMem *mem, NicRing **ring, GeneralTaskFifo *fifos);

int fuselink_recv(void* buffer, size_t size, cudaStream_t stream);

int fuselink_init(char *remote_ip, int remote_port);

#endif