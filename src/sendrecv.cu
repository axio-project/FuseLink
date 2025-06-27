#include "sendrecv.cuh"
#include <cuda.h>
#include "checks.h"
#include "channel.h"

__device__ __forceinline__ uint64_t ld_volatile_device(uint64_t* ptr) {
  uint64_t ans;
  asm volatile("ld.volatile.global.u64 %0, [%1];"
               : "=l"(ans)
               : "l"(ptr)
               : "memory");
  return ans;
}

__device__ __forceinline__ void st_volatile_device(uint64_t* ptr, uint64_t val) {
  asm volatile("st.volatile.global.u64 [%0], %1;"
               :
               : "l"(ptr), "l"(val)
               : "memory");
}

int fuselink_send(void* buffer, size_t size, cudaStream_t stream, int peer_id) {
  // check if buffer on gpu
  CUdeviceptr d_ptr = (CUdeviceptr)buffer;
  CUcontext ctx = 0;
  CUresult res = cuPointerGetAttribute(&ctx, CU_POINTER_ATTRIBUTE_CONTEXT, d_ptr);
  FUSELINK_CHECK_CUDA_ERR(res, "pointer not on gpu");
  
  // partition buffer into chunks
  size_t n_chunks = CEIL_DIV(size, FUSELINK_CHUNK_SZ);

  // allocate channels
  SendChannel *chs[N_CHANNELS];

  // TODO: allocate channels
  // allocate_channels(peer_id, N_CHANNELS, (void**)chs, CHANNEL_TYPE_SEND);
  for (size_t i = 0; i < N_CHANNELS; i++) {
    chs[i] = new SendChannel();
  }

  // calculate the number of tasks for each channel
  int ntasks[N_CHANNELS];
  for (size_t i = 0; i < N_CHANNELS; i++) {
    ntasks[i] = FLOOR_DIV(n_chunks, N_CHANNELS);
    if (i < n_chunks % N_CHANNELS) {
      ntasks[i]++;
    }
  }
  printf("ntasks: ");
  for (size_t i = 0; i < N_CHANNELS; i++) {
    printf("%d ", ntasks[i]);
  }
  printf("\n");

  // may be blocking
  // set up threads for channels
  std::thread threads[N_CHANNELS];
  for (size_t i = 0; i < N_CHANNELS; i++) {
    threads[i] = std::thread(&SendChannel::threadMain, chs[i]);
  }


  // allocate GpuTaskFifo* fifos in unified memory, freed in the end
  GpuTaskFifo *fifos;
  CUDA_CHECK(cudaMallocManaged(&fifos, N_CHANNELS * sizeof(GpuTaskFifo)));
  CUDA_CHECK(cudaMemset(fifos, 0, N_CHANNELS * sizeof(GpuTaskFifo)));

  // count GPU nums
  int nGpus = 0;
  CUDA_CHECK(cudaGetDeviceCount(&nGpus));
  
  // build rings, to be freed in the end
  NicRing* rings[N_CHANNELS];
  for (size_t i = 0; i < N_CHANNELS; i++) {
    rings[i] = new NicRing();
    initNicRing(rings[i], 0);
  }

  // setup task dispatcher thread
  int nchannels = N_CHANNELS;
  std::thread task_dispatcher_thread(&send_task_dispatcher, buffer, size, chs, fifos, rings, nchannels);

  // run kernel
  int threads_per_block = 1024;
  cudaEvent_t start, end;
  CUDA_CHECK(cudaEventCreate(&start));
  CUDA_CHECK(cudaEventCreate(&end));
  cudaEventRecord(start, stream);
  
  fuselink_send_kernel<<<N_CHANNELS, threads_per_block, 0, stream>>>(buffer, size, NULL, NULL, fifos);
  CUDA_CHECK(cudaEventRecord(end, stream));
  CUDA_CHECK(cudaEventSynchronize(end));
  float elapsed_time;
  CUDA_CHECK(cudaEventElapsedTime(&elapsed_time, start, end));
  printf("copy time: %f ms\n", elapsed_time);
  CUDA_CHECK(cudaEventDestroy(start));
  CUDA_CHECK(cudaEventDestroy(end));
  
  // join cpu thread
  task_dispatcher_thread.join();
  printf("task dispatcher thread joined\n");
  
  // wait for channels to finish
  for (size_t i = 0; i < N_CHANNELS; i++) {
    threads[i].join();
  }

  // free channels
  for (size_t i = 0; i < N_CHANNELS; i++) {
    delete chs[i];
  }

  // free rings
  for (size_t i = 0; i < N_CHANNELS; i++) {
    freeNicRing(rings[i]);
    delete rings[i];
  }

  CUDA_CHECK(cudaFree(fifos));

  return 0;

}

int fuselink_send_general(void* buffer, size_t size, cudaStream_t stream, int peer_id, bool verify) {
  // check if buffer on gpu
  CUdeviceptr d_ptr = (CUdeviceptr)buffer;
  CUcontext ctx = 0;
  CUresult res = cuPointerGetAttribute(&ctx, CU_POINTER_ATTRIBUTE_CONTEXT, d_ptr);
  FUSELINK_CHECK_CUDA_ERR(res, "pointer not on gpu");
  
  // partition buffer into chunks
  size_t n_chunks = CEIL_DIV(size, FUSELINK_CHUNK_SZ);

  // allocate channels
  SendChannel *chs[N_CHANNELS];

  // TODO: allocate channels
  // allocate_channels(peer_id, N_CHANNELS, (void**)chs, CHANNEL_TYPE_SEND);
  for (size_t i = 0; i < N_CHANNELS; i++) {
    chs[i] = new SendChannel();
  }

  // calculate the number of tasks for each channel
  int ntasks[N_CHANNELS];
  for (size_t i = 0; i < N_CHANNELS; i++) {
    ntasks[i] = FLOOR_DIV(n_chunks, N_CHANNELS);
    if (i < n_chunks % N_CHANNELS) {
      ntasks[i]++;
    }
  }
  printf("ntasks: ");
  for (size_t i = 0; i < N_CHANNELS; i++) {
    printf("%d ", ntasks[i]);
  }
  printf("\n");

  // allocate GpuTaskFifo* fifos in unified memory, freed in the end
  GeneralTaskFifo *fifos;
  // CUDA_CHECK(cudaMallocManaged(&fifos, N_CHANNELS * sizeof(GeneralTaskFifo)));
  CUDA_CHECK(cudaMallocHost(&fifos, N_CHANNELS * sizeof(GeneralTaskFifo), cudaHostAllocMapped));
  CUDA_CHECK(cudaMemset(fifos, 0, N_CHANNELS * sizeof(GeneralTaskFifo)));

  for (uint i = 0; i < N_CHANNELS; i++) {
    chs[i]->setGeneralTaskFifo(&fifos[i]);
  }
  // count GPU nums
  int nGpus = 0;
  CUDA_CHECK(cudaGetDeviceCount(&nGpus));
  
  // build rings, to be freed in the end
  NicRing* rings[N_CHANNELS];
  for (size_t i = 0; i < N_CHANNELS; i++) {
    rings[i] = new NicRing();
    initNicRing(rings[i], 0);
  }

  // setup task dispatcher thread
  int nchannels = N_CHANNELS;
  std::thread general_task_dispatcher_thread(&send_task_dispatcher_general, buffer, size, chs, fifos, rings, nchannels, verify);

  // run kernel
  int threads_per_block = 1024;
  dim3 grid_dim(N_CHANNELS, N_BLOCK_PER_FIFO);
  cudaEvent_t start, end;
  CUDA_CHECK(cudaEventCreate(&start));
  CUDA_CHECK(cudaEventCreate(&end));
  cudaEventRecord(start, stream);
  
  fuselink_send_general_kernel<<<grid_dim, threads_per_block, 0, stream>>>(buffer, size, NULL, NULL, fifos);
  CUDA_CHECK(cudaEventRecord(end, stream));
  CUDA_CHECK(cudaEventSynchronize(end));
  float elapsed_time;
  CUDA_CHECK(cudaEventElapsedTime(&elapsed_time, start, end));
  printf("copy time: %f ms\n", elapsed_time);
  CUDA_CHECK(cudaEventDestroy(start));
  CUDA_CHECK(cudaEventDestroy(end));
  
  // join cpu thread
  general_task_dispatcher_thread.join();
  printf("task dispatcher thread joined\n");
  
  // free channels
  for (size_t i = 0; i < N_CHANNELS; i++) {
    delete chs[i];
  }

  // free rings
  for (size_t i = 0; i < N_CHANNELS; i++) {
    freeNicRing(rings[i]);
    delete rings[i];
  }

  CUDA_CHECK(cudaFreeHost(fifos));

  return 0;
}

__global__ void fuselink_send_kernel(void *buffer, size_t size, GpuMem *mem, NicRing **ring, GpuTaskFifo *fifos) {
  // get the current channel
  const int block_id = gridDim.x * blockIdx.y + blockIdx.x;
  const int thread_id = threadIdx.x;
  const int nblocks = gridDim.x;
  const int nthreads = blockDim.x;

  GpuTaskFifo* fifo = &fifos[block_id];

  // write to the buffer
  int ntasks = CEIL_DIV(size, FUSELINK_CHUNK_SZ);

  // calculate the number of tasks for this block
  int ntasks_my_block = ntasks / nblocks;
  if (block_id < ntasks % nblocks) {
    ntasks_my_block++;
  }

  // to optimize access speed, the threads within a block should handle a continuous chunk of tasks
  // calculate the start address, in bytes
  size_t offset = block_id * FUSELINK_CHUNK_SZ;
  // task size in bytes
  size_t task_size = FUSELINK_CHUNK_SZ;

  size_t thread_step_size = nthreads; // each thread copy a uint64_t at a time
  size_t nsteps_per_thread = CEIL_DIV(task_size, thread_step_size * sizeof(uint64_t));
  uint64_t *cur_src_addr = (uint64_t *) ((uintptr_t)buffer + offset);

  for (size_t i = 0; i < ntasks_my_block;/*update in function*/) {
    // wait for available task slot
    if (fifo->fifo_flags[fifo->tail % FIFO_SZ] <= 0) {
      continue;
    }
    __syncthreads();
    uint64_t *cur_dst_addr = (uint64_t *) ((uintptr_t)fifo->tasks[fifo->tail % FIFO_SZ].buffer);

    // copy the task to the buffer
    for (size_t j = 0; j < nsteps_per_thread; j++) {
      // printf("copy thread %d, block %d, step %d\n", thread_id, block_id, j);
      cur_dst_addr[thread_id] = cur_src_addr[thread_id];
      cur_src_addr += thread_step_size;
      cur_dst_addr += thread_step_size;
      __syncthreads();
    }
    // sync the threads within the block
    __syncthreads();
    // post to the channel
    // if (thread_id == 0 && block_id == 0) {
    //   printf("post to channel\n");
    // }
    if (thread_id == 0) {
      fifo->tasks[fifo->tail % FIFO_SZ].ready_flag = 1;
      fifo->fifo_flags[fifo->tail % FIFO_SZ] = -1; // notify the dispatcher thread
      fifo->tail++;
    }
    i++;
  }
  
}



__global__ void fuselink_send_general_kernel(void *buffer, size_t size, GpuMem *mem, NicRing **ring, GeneralTaskFifo *fifos) {
  // get the current channel
  const int channel_id = blockIdx.x;
  const int block_y = blockIdx.y;
  const int thread_id = threadIdx.x;
  const int thread_id_in_channel = thread_id + blockDim.x * block_y;
  
  const int my_fifo = channel_id;

  // printf("block_shape %d, %d, %d block_id %d, %d, %d\n", gridDim.x, gridDim.y, gridDim.z, blockIdx.x, blockIdx.y, blockIdx.z);

  uint64_t read_idx = 0;
  uint64_t write_idx = 0;


  GeneralTaskFifo* fifo = &fifos[my_fifo];

  // write to the buffer
  int ntasks = CEIL_DIV(size, FUSELINK_CHUNK_SZ);

  // calculate the number of tasks for this block
  int ntasks_my_block = ntasks / N_CHANNELS;
  
  if (channel_id < ntasks % N_CHANNELS) {
    ntasks_my_block++;
  }

  // to optimize access speed, the threads within a block should handle a continuous chunk of tasks
  // calculate the start address, in bytes
  size_t offset = my_fifo * FUSELINK_CHUNK_SZ;
  size_t channel_step_sz = FUSELINK_CHUNK_SZ * N_CHANNELS / sizeof(uint32_t);
  // task size in bytes
  size_t task_size = FUSELINK_CHUNK_SZ;

  size_t thread_step_size = gridDim.y * blockDim.x; // each thread copy a uint32_t at a time
  size_t nsteps_per_thread = CEIL_DIV(task_size, thread_step_size * sizeof(uint32_t));
  uint32_t *slot_src_addr = (uint32_t *) ((uintptr_t)buffer + offset);
  // printf("thread_step_size %lu, nsteps_per_thread %lu fifo %d, slot_src_addr %p buffer %p\n", thread_step_size, nsteps_per_thread, my_fifo, slot_src_addr, buffer);
  for (size_t i = 0; i < ntasks_my_block;/*update i in function only after successfull*/) {
    // wait for available task slot
    // if (thread_id == 0) {
    write_idx = ld_volatile_device(&fifo->head);
      // printf("i %d, write_idx %lu\n", i, write_idx);
    // }
    // __syncthreads();
    for (auto t = read_idx; t < write_idx; t++) {
      auto& task = fifo->tasks[t % FIFO_SZ];
      uint32_t *cur_dst_addr = (uint32_t *) ((uintptr_t)task.buffer);

      // copy the task to the buffer
      uint32_t *cur_src_addr = slot_src_addr;
      #pragma unroll
      for (size_t j = 0; j < nsteps_per_thread; j++) {
        cur_dst_addr[thread_id_in_channel] = cur_src_addr[thread_id_in_channel];
        cur_src_addr += thread_step_size;
        cur_dst_addr += thread_step_size;
      }
      i++;
      slot_src_addr += channel_step_sz;
      __syncthreads();
    }
    __threadfence_block();
    if (thread_id == 0 && write_idx > read_idx) {
      // printf("block_id %lu, write idx %lu\n", block_id, write_idx);
      // fifo->tail_per_block[block_id % N_BLOCK_PER_FIFO] = write_idx;
      fifo->tail = write_idx;
    }
    read_idx = write_idx;
  }
}

int fuselink_recv(void* buffer, size_t size, cudaStream_t stream) {
  return 0;
}

