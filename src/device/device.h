#ifndef FUSELINK_DEVICE_H
#define FUSELINK_DEVICE_H

#include <cstdint>
// #include <infiniband/verbs.h>
#include <cuda.h>
#include "debug.h"

#define RING_SIZE 16

struct GpuMem {
  uint32_t device_id;
  uint64_t size;
  CUdeviceptr device_ptr;
  CUmemGenericAllocationHandle allocation_handle;
  struct ibv_mr* mr;
  int in_use; // 0: not in use, 1: in use
};

struct GpuMemPool { // with device id
  uint32_t device_id;
  GpuMem* mems;
  uint32_t num_mems;
};

struct NicRing {
  uint32_t nic_id;
  // gpu mem
  GpuMem* gpu_mems[RING_SIZE];
  int head; // consumed by dispatcher thread
  int tail; // produced after released
};

void initGpuMem(GpuMem** gmem, size_t size, uint32_t device_id);

void freeGpuMem(GpuMem* mem);

void initNicRing(NicRing* ring, uint32_t nic_id);

void freeNicRing(NicRing* ring);

int allocRingSlot(NicRing* ring, GpuMem* mem);

void deallocRingSlot(NicRing* ring, int slot);

#endif