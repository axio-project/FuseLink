#include "channel.h"
#include "sendrecv.cuh"
#include <unistd.h>
#include <vector>
#include <chrono>

// Initialize global channel arrays
SendChannel* global_send_channels[MAX_PEERS][N_CHANNELS] = {nullptr};
RecvChannel* global_recv_channels[MAX_PEERS][N_CHANNELS] = {nullptr};


class SimulateTask {
public:
  SimulateTask(int ring_id, int slot) {
    start_time = std::chrono::high_resolution_clock::now();
    this->ring_id = ring_id;
    this->slot = slot;
  }

  bool finished() {
    return true;
    // return std::chrono::high_resolution_clock::now() - start_time > std::chrono::milliseconds(1);
  }

  int ring_id;
  std::chrono::time_point<std::chrono::high_resolution_clock> start_time;
  int slot;
};

inline uint64_t load_volatile_host(uint64_t volatile* ptr) {
  uint64_t ans;
  asm volatile("movq %1, %0" : "=r"(ans) : "m"(*ptr) : "memory");
  return ans;
}

inline uint64_t load_fifo_tail(uint64_t* fifo_tail, int nblocks) {
  uint64_t ret = UINT64_MAX;
  for (int i = 0; i < nblocks; i++) {
    auto tail = load_volatile_host(&fifo_tail[i]);
    if (tail < ret) {
      ret = tail;
    }
  }
  return ret;
}

// // Channel class implementation
// int Channel::getNewTask() {
//     return 0; // TODO: implement
// }

// int Channel::commitTask() {
//     return 0; // TODO: implement
// }

// SendChannel class implementation
SendChannel::SendChannel() : Channel() {
    // Base class constructor handles initialization
}

SendChannel::~SendChannel() {
    // Base class destructor handles cleanup
}

void SendChannel::threadMain() {
  while (true) {
    // Wait for work
    std::unique_lock<std::mutex> lock(_mutex);
    _cv.wait(lock, [this]() {
        return _state == ChannelState::WORKING || _state == ChannelState::DONE;
    });
    if (_state == ChannelState::DONE) {
      return;
    }

    // Process tasks until done
    while (_task_fifo->head > _task_fifo->tail) { // have task to process
      // Get the task
      __sync_synchronize();  // Ensure we see the latest values
      const int idx = _task_fifo->tail % FIFO_SZ;
      CpuTask& task = _task_fifo->tasks[idx];
        
      // Process the task
      // TODO: Implement actual data transmission here
      // This is where you would handle the actual data transfer
      // using the task.buffer and task.buffer_size
      
      // Mark slot as empty
      _task_fifo->tail++;
      __sync_synchronize();  // Ensure the tail update is visible
      _processed_tasks++;
        
      printf("Processed task %d (%d/%d)\n", 
              idx, _processed_tasks, _total_tasks);
    }
  }
}

void SendChannel::threadMain_general() {
  while (true) {
    // Wait for work
    std::unique_lock<std::mutex> lock(_mutex);
    _cv.wait(lock, [this]() {
        return _state == ChannelState::WORKING || _state == ChannelState::DONE;
    });
    if (_state == ChannelState::DONE) {
      return;
    }

    // Process all tasks in general fifo
    auto tail = load_volatile_host(&_general_task_fifo->tail);
    auto head = load_volatile_host(&_general_task_fifo->head);
    // printf("tail %d, head %d\n", tail, head);
    for (int i = tail; i < head; i++) {
      GeneralTask& task = _general_task_fifo->tasks[i % FIFO_SZ];
      // Process the task
      auto stage = task.stage;
      if (stage == TASK_STAGE_TRANSMIT) {
        // DO THE RDMA WRITE WITH IMM
        printf("do the rdma write with imm\n");
        task.stage = TASK_STAGE_TRANSMIT_PENDING;
      } else if (stage == TASK_STAGE_TRANSMIT_PENDING) {
        printf("poll completion queue\n");
        // POLL COMPLETION QUEUE
        // If completed:
        task.stage = TASK_STAGE_FINISH;
        printf("task finished\n");
      }
    }
  }
}

int SendChannel::checkSendingStatus() {
  // TODO: implement with verbs
  int ncompleted = 0;
  for (int i = _task_fifo->tail; i < _task_fifo->head; i++) {
    const int idx = i % FIFO_SZ;
    if (*(_task_fifo->tasks[idx].ready_flag) == 1) {
      ncompleted++;
    }
    _task_fifo->transmit_complete_flag = 1;
  }
  return ncompleted;
}

// RecvChannel class implementation
RecvChannel::RecvChannel() : Channel() {
    // Base class constructor handles initialization
}

RecvChannel::~RecvChannel() {
    // Base class destructor handles cleanup
}

void RecvChannel::threadMain() {
    // TODO: implement
}

void RecvChannel::threadMain_general() {

  // TODO: implement

}

// Channel allocation function
void allocate_channels(int peer_id, int n_channels, void** channel_array, int channel_type) {
  if (channel_type == CHANNEL_TYPE_SEND) {
    for (int i = 0; i < n_channels; i++) {
        global_send_channels[peer_id][i] = new SendChannel();
        channel_array[i] = global_send_channels[peer_id][i];
    }
  } else {
    for (int i = 0; i < n_channels; i++) {
        global_recv_channels[peer_id][i] = new RecvChannel();
        channel_array[i] = global_recv_channels[peer_id][i];
    }
  }
}


void send_task_dispatcher(void* buffer, size_t sz, SendChannel* channels[], GpuTaskFifo fifos[], NicRing* rings[], int n_channels) {
  const int batch_sz = 8; // schedule 8 chunks to a set of rings/channels
  const int nchunks = CEIL_DIV(sz, FUSELINK_CHUNK_SZ);

  CpuTask* cpu_task_list[8][8] = {nullptr}; // allow at most 8 outstanding tasks for 8 channels

  int gpu_idx = 0;
  int pending_tasks = 0;
  for (int i = 0; i < nchunks; /*update i only if send a task*/) { // every chunk is scheduled to a ring and a channel
    
    if (i % 32 == 0 && i > 0) { // check pending tasks every 32 chunks
      for (int j = 0; j < n_channels; j++) {
        int ncompleted = channels[j]->checkSendingStatus();
        pending_tasks -= ncompleted;
        printf("Channel %d: %d tasks completed\n", j, ncompleted);
        if (ncompleted > 0) {
          // dealloc the ring slot in the channel
          for (int k = 0; k < batch_sz; k++) {
            if (cpu_task_list[j][k] != nullptr && cpu_task_list[j][k]->transmit_complete_flag == 1) {
              int ring = cpu_task_list[j][k]->ring_id;
              int slot = cpu_task_list[j][k]->buffer_slot;
              deallocRingSlot(rings[ring], slot);
              cpu_task_list[j][k] = nullptr;
            } // if
          }// for all slots in the channel
        } // if ncompleted > 0
      } // for all channels
    }

    int target_channel_idx = -1;
    int target_batch_offset = -1;

    // find a free slot in the channel
    for (int p = 0; p < n_channels; p++) {
      for (int q = 0; q < batch_sz; q++) {
        if (cpu_task_list[p][q] == nullptr || cpu_task_list[p][q]->transmit_complete_flag == 1) {
          target_channel_idx = p;
          target_batch_offset = q;
          break;
        }
      }
    }
    if (target_channel_idx == -1) {
      // channels full of pending tasks
      continue;
    }

    // every chunk is scheduled to a ring
    int target_ring_idx = target_channel_idx;

    NicRing* target_ring = rings[target_ring_idx];
    GpuMem mem;
    int ring_slot = 0;
    int ret = allocRingSlot(target_ring, &mem);
    printf("write to fifo %d, head %d, mem ptr %p\n", gpu_idx, fifos[gpu_idx].head, mem.device_ptr);
    if (ret < 0) {
      // report error and return
      printf("No free slot in the ring %d\n", target_ring_idx);
      exit(1);
    }
    // build cpu task and gpu task with the ring slot
    GpuTask& gpu_task = fifos[gpu_idx].tasks[fifos[gpu_idx].head % FIFO_SZ];
    CpuTask cpu_task;

    size_t chunk_sz = i == nchunks - 1 ? sz - i * FUSELINK_CHUNK_SZ : FUSELINK_CHUNK_SZ;

    gpu_task.buffer = (void *)mem.device_ptr;
    gpu_task.buffer_size = chunk_sz; 
    gpu_task.ready_flag = 0;
    gpu_task.transmit_complete_flag = 0;

    // schedule to GpuTaskFifo[gpu_idx]
    // write to head, read from tail
    // ready to be consumed by GPU
    fifos[gpu_idx].fifo_flags[fifos[gpu_idx].head % FIFO_SZ] = 1;
    fifos[gpu_idx].head++;

    cpu_task.buffer = (void *)mem.device_ptr;
    cpu_task.buffer_size = chunk_sz;
    cpu_task.ring_id = target_ring_idx;
    cpu_task.buffer_slot = ret;
    cpu_task.ready_flag = &gpu_task.ready_flag;
    cpu_task.transmit_complete_flag = 0;

    // schedule to CpuTaskFifo[target_channel]
    cpu_task_list[target_channel_idx][target_batch_offset] = channels[target_ring_idx]->setTask(cpu_task);

    // sleep(1);
    // printf("get task ready flag %d\n", *cpu_task_list[target_channel_idx][target_batch_offset]->ready_flag);
    
    // update pending tasks
    pending_tasks++;
    
    // go to the next gpu;
    gpu_idx = (gpu_idx + 1) % n_channels;
    i++; // update i only if send a task
  }
  
  while (pending_tasks > 0) { // remaining tasks
    for (int i = 0; i < n_channels; i++) {
      int ncompleted = channels[i]->checkSendingStatus();
      pending_tasks -= ncompleted;
      if (ncompleted > 0) {
        printf("Channel %d: %d tasks completed, pending %d tasks\n", i, ncompleted, pending_tasks);
      }
      if (pending_tasks == 0) {
        printf("all tasks completed\n");
        for (int i = 0; i < n_channels; i++) {
          channels[i]->setDoneIfAllFinished();
        }
        break;
      }
    }
  }

}

bool verifyData(uint32_t* data, size_t size, uint64_t start_idx) {
  void* h_data = malloc(size);
  cudaMemcpy(h_data, data, size, cudaMemcpyDeviceToHost);
  uint32_t* data_host = (uint32_t*)h_data;
  bool result = true;
  for (size_t i = 0; i < size / sizeof(uint32_t); i++) {
    if (data_host[i] != start_idx + i) {
      printf("Data mismatch at index %d: expected %d, got %d\n", i, start_idx + i, data_host[i]);
      result = false;
      break;
    }
  }
  free(h_data);
  return result;
}


void send_task_dispatcher_general(void* buffer, size_t sz, SendChannel* channels[], GeneralTaskFifo fifos[], NicRing* rings[], int n_channels, bool verify) {
  const int batch_sz = 8;
  const int nchunks = CEIL_DIV(sz, FUSELINK_CHUNK_SZ);

  int pending_tasks = 0;
  int sent_tasks = 0;
  int tail_cache[N_CHANNELS] = {0};
  std::vector<int> channel_pending_tasks[N_CHANNELS];
  std::vector<SimulateTask> channel_pending_tasks_sim[N_CHANNELS];

  bool check_flag = false;
  for (int i = 0; i < nchunks;) {
    // find an available channel
    int target_channel_idx = i % n_channels;
    int target_ring_idx = (i / n_channels) % n_channels;

    NicRing* target_ring = rings[target_ring_idx];
    GpuMem mem;
    int ring_slot = allocRingSlot(target_ring, &mem);
    // printf("allocRingSlot: %d, %d\n", target_ring_idx, ring_slot);

    if (ring_slot < 0) {
      // printf("No free slot in the ring %d\n", target_ring_idx);
      check_flag = true;
    }

    // build general task and gpu task with the ring slot
    if (ring_slot >= 0) {
      GeneralTask general_task;
      general_task.buffer = (void *)mem.device_ptr;
      size_t chunk_sz = i == nchunks - 1 ? sz - i * FUSELINK_CHUNK_SZ : FUSELINK_CHUNK_SZ;
      general_task.buffer_size = chunk_sz;
      general_task.ring_id = target_ring_idx;
      general_task.buffer_slot = ring_slot;
      general_task.stage = TASK_STAGE_COPY;
      general_task.chunk_id = i;

      // set general task
      channels[target_channel_idx]->setTask_general(general_task);

      // update i only if send a task
      i++;
      pending_tasks++;
    }
    
    if (pending_tasks >= 96 || check_flag) {
      for (int i = 0; i < n_channels; i++) {
        // check all tasks in the channel
        // auto tail = load_fifo_tail(fifos[i].tail_per_block, N_BLOCK_PER_FIFO);
        auto tail = load_volatile_host(&fifos[i].tail);
        if (tail > tail_cache[i]) {
          printf("channel %d: tail %lu, tail_cache %lu\n", i, tail, tail_cache[i]);
          for (int j = tail_cache[i]; j < tail; j++) {
            // post send on task j

            // push to pending_tasks[i]
            // printf("post send on task %d\n", j);
            // read target ring and slot
            int target_ring_idx = fifos[i].tasks[j % FIFO_SZ].ring_id;
            int target_ring_slot = fifos[i].tasks[j % FIFO_SZ].buffer_slot;
            channel_pending_tasks_sim[i].push_back(SimulateTask(target_ring_idx, target_ring_slot));
            sent_tasks++;
            // todo select ring and post send
          }
          // update tail cache
          tail_cache[i] = tail;
        }
      }
      // check if any posted tasks has finished
      // channel[i]->checkSendingFinished();
      // mark finished
      // free the ring slot
      for (int i = 0; i < n_channels; i++) {
        for (int j = 0; j < channel_pending_tasks_sim[i].size(); j++) {
          auto& task = channel_pending_tasks_sim[i][j];
          if (task.finished()) {
            // mark finished
            // free the ring slot
            // printf("task finished deallocRingSlot: %d, %d\n", task.ring_id, task.slot);
            deallocRingSlot(rings[task.ring_id], task.slot);
            // remove this task from channel_pending_tasks_sim
            pending_tasks--;
            channel_pending_tasks_sim[i].erase(channel_pending_tasks_sim[i].begin() + j);
          }
        }
      }
      check_flag = false;
    } // check pending tasks has finished or not
    
  }// for all chunks

  printf("sent %d tasks\n", sent_tasks);

  while (sent_tasks < nchunks) { // send remaining tasks
    // check if any posted tasks has copy ready
    for (int i = 0; i < n_channels; i++) {
      // auto tail = load_fifo_tail(fifos[i].tail_per_block, N_BLOCK_PER_FIFO);
      auto tail = load_volatile_host(&fifos[i].tail);
      printf("channel %d: now tail %lu\n", i, tail);
      if (tail > tail_cache[i]) {
        printf("channel %d: tail %lu, tail_cache %lu\n", i, tail, tail_cache[i]);
        for (int j = tail_cache[i]; j < tail; j++) {
          // check if any posted tasks has copy ready
          // mark finished
          // check data
          if (verify) {
            auto &task = fifos[i].tasks[j % FIFO_SZ];
            uint64_t start_idx = task.chunk_id * FUSELINK_CHUNK_SZ / sizeof(uint32_t);
            uint32_t* data = (uint32_t*)task.buffer;
            printf("verify data at chunk id %d, start_idx %d\n", task.chunk_id, start_idx);
            if (!verifyData(data, FUSELINK_CHUNK_SZ, start_idx)) {
              printf("Data mismatch at index %d\n", j);
              exit(1);
            } else {
              printf("Data verified at index %d\n", j);
            }
          }
          auto& task = fifos[i].tasks[j % FIFO_SZ];
          channel_pending_tasks_sim[i].push_back(SimulateTask(task.ring_id, task.buffer_slot));
          sent_tasks++;
        }
      }
      tail_cache[i] = tail;
    }
  }

  while (pending_tasks > 0) {
    for (int i = 0; i < n_channels; i++) {
      if (channel_pending_tasks_sim[i].size() > 0) {
        // check if any posted tasks has finished
        for (int j = 0; j < channel_pending_tasks_sim[i].size(); j++) {
          auto& task = channel_pending_tasks_sim[i][j];
          if (task.finished()) {
            // free the ring slot
            // printf("task finished\n");
            deallocRingSlot(rings[task.ring_id], task.slot);
            pending_tasks--;
            channel_pending_tasks_sim[i].erase(channel_pending_tasks_sim[i].begin() + j);
          }
        }
      }
    }
  }

}

void recv_task_dispatcher(void* buffer, size_t sz, RecvChannel* channels[], void* rings[], int n_channels) {
  // TODO: implement
}