#include "device.h"
#include <cuda_runtime.h>
#include <cuda.h>
#include <stdio.h>

void initGpuMem(GpuMem** gmem, size_t size, uint32_t device_id) {
  CUDA_CHECK(cudaSetDevice(device_id));
  CUDA_CHECK(cudaMallocManaged((void**)gmem, sizeof(GpuMem)));
  GpuMem* mem = *gmem;
  mem->device_id = device_id;
  mem->size = size;
  mem->mr = nullptr; // TODO: register mr
  // allocate memory
  // using cu mem api
  size_t granularity = 0;
  size_t aligned_sz = 0;
  CUmemAllocationProp prop = {};
  prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
  prop.location.id = device_id;
  prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
  prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;

  CUCHECK(cuMemGetAllocationGranularity(&granularity, &prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM));
  aligned_sz = ((size + granularity - 1) / granularity) * granularity;
  mem->size = aligned_sz;
  // printf("aligned_sz %zu\n", aligned_sz);

  // create physical memory
  CUCHECK(cuMemCreate(&mem->allocation_handle, aligned_sz, &prop, 0));
  
  // address reserve
  CUCHECK(cuMemAddressReserve(&mem->device_ptr, aligned_sz, 0, 0, 0));

  // map
  CUCHECK(cuMemMap(mem->device_ptr, aligned_sz, 0, mem->allocation_handle, 0));

  // set access
  CUmemAccessDesc accessDesc = {};
  accessDesc.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
  accessDesc.location.id = device_id;
  accessDesc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
  CUCHECK(cuMemSetAccess(mem->device_ptr, aligned_sz, &accessDesc, 1));

}

void freeGpuMem(GpuMem* mem) {
  CUCHECK(cuMemUnmap(mem->device_ptr, mem->size));
  CUCHECK(cuMemRelease(mem->allocation_handle));
  CUCHECK(cuMemAddressFree(mem->device_ptr, mem->size));
  CUDA_CHECK(cudaFree(mem));
}

void initNicRing(NicRing* ring, uint32_t nic_id) {
  ring->nic_id = nic_id;
  ring->head = 0;
  ring->tail = 0;
  // initialize gpu mems
  for (int i = 0; i < RING_SIZE; i++) {
    // TODO: Replace with a macro
    initGpuMem(&ring->gpu_mems[i], FUSELINK_CHUNK_SZ, nic_id);
  }
}

void freeNicRing(NicRing* ring) {
  if (ring->head != ring->tail) {
    printf("Warning: NicRing is not empty\n");
  }
  for (int i = 0; i < RING_SIZE; i++) {
    freeGpuMem(ring->gpu_mems[i]);
  }
}

int allocRingSlot(NicRing* ring, GpuMem* mem) {
  // int next_slot = ring->head;
  int target_slot = -1;
  for (int i = 0; i < RING_SIZE; i++) {
    if (!ring->gpu_mems[i]->in_use) {
      target_slot = i;
      break;
    }
  }
  if (target_slot == -1) {
    // error
    // printf("Error: NicRing is full\n");
    return -1;
  }
  ring->gpu_mems[target_slot]->in_use = 1;

  *mem = *ring->gpu_mems[target_slot];
  return target_slot;
}

void deallocRingSlot(NicRing* ring, int slot) {
  ring->gpu_mems[slot]->in_use = 0;
}
