#ifndef FUSELINK_TASK_H
#define FUSELINK_TASK_H

#include <cstddef>
#include <cuda_runtime.h>
#include <cuda.h>
// #include <cuda>

#define FIFO_SZ 32
#define N_BLOCK_PER_FIFO 8

enum TaskStage {
  TASK_STAGE_COPY, // ready to copy
  TASK_STAGE_TRANSMIT, // ready to transmit via RDMA
  TASK_STAGE_TRANSMIT_PENDING, // waiting for RDMA to complete
  TASK_STAGE_RECEIVED, // received by remote
  TASK_STAGE_FINISH, // transmit done or consume done
  TASK_STAGE_INVALID, // empty slot, should not be in tail <-> head window
};

struct GeneralTask {
  void* buffer;
  size_t buffer_size;
  unsigned int ring_id; // belongs to which ring (NIC)
  unsigned int buffer_slot;
  TaskStage stage;
  int chunk_id;
  alignas(128) char padding[128];  // Ensure cache line alignment
};

struct alignas(128) GeneralTaskFifo {
  alignas(128) GeneralTask tasks[FIFO_SZ];
  uint64_t head;
  volatile uint64_t tail; // both head and tail is updated on control thread
  uint64_t tail_per_block[N_BLOCK_PER_FIFO];
  alignas(128) char padding[128];  // Ensure cache line alignment
};


struct GpuTask {
  void* buffer; // GPU write to this buffer
  size_t buffer_size;  // Size of the buffer in bytes
  volatile int ready_flag;
  volatile int transmit_complete_flag; // in recv, this flag is set to 1 to indicate GPU has copied the data to the recv buffer
  alignas(64) char padding[64];  // Ensure cache line alignment
};

struct GpuTaskFifo { // consumed by GPU and filled by CPU
  alignas(64) GpuTask tasks[FIFO_SZ];
  int head;
  int tail;
  volatile int fifo_flags[FIFO_SZ]; // -1: empty, 1: ready
  alignas(64) char padding[64];  // Ensure cache line alignment
};

struct CpuTask {
  void* buffer; // RDMA this buffer to remote or remote write to this buffer
  size_t buffer_size;
  int ring_id; // belongs to which ring (NIC)
  int buffer_slot; // belongs to which buffer slot
  volatile int* ready_flag; // access by cpu to indicate the data is ready
  volatile int transmit_complete_flag; // in send, this flag is set to 1 to indicate the CPU thread has finished sending/receiving
};

struct CpuTaskFifo { // consumed by CPU, filled by CPU and data filled by GPU
  CpuTask tasks[FIFO_SZ];
  int head;
  int tail;
  volatile int transmit_complete_flag;
};

#endif