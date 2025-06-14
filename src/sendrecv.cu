#include "sendrecv.cuh"
#include <cuda.h>
#include "checks.h"
#include "channel.h"


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
  size_t step_size = nblocks * FUSELINK_CHUNK_SZ;
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

int fuselink_recv(void* buffer, size_t size, cudaStream_t stream) {
  return 0;
}

