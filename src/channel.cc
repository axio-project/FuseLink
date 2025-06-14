#include "channel.h"
#include "sendrecv.cuh"
#include <unistd.h>

// Initialize global channel arrays
SendChannel* global_send_channels[MAX_PEERS][N_CHANNELS] = {nullptr};
RecvChannel* global_recv_channels[MAX_PEERS][N_CHANNELS] = {nullptr};

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
    while (_task_fifo->fifo_flags[_task_fifo->tail % FIFO_SZ] == 1) { // have task to process
      int idx = _task_fifo->tail % FIFO_SZ;
      int flag = _task_fifo->fifo_flags[idx];
      
      if (flag == 1 && *(_task_fifo->tasks[idx].ready_flag) == 1) {
        // Get the task
        CpuTask& task = _task_fifo->tasks[idx];
        
        // Process the task
        // TODO: Implement actual data transmission here
        // This is where you would handle the actual data transfer
        // using the task.buffer and task.buffer_size
        
        // Mark slot as empty
        // _task_fifo->fifo_flags[idx] = -1;
        _task_fifo->tail++;
        _processed_tasks++;
        
        printf("Processed task %d (%d/%d)\n", 
                idx, _processed_tasks, _total_tasks);
      }
    }
  }
}

int SendChannel::checkSendingStatus() {
  // TODO: implement with verbs
  int ncompleted = 0;
  for (int i = 0; i < FIFO_SZ; i++) {
    if (_task_fifo->fifo_flags[i] == 1) {
      if (*(_task_fifo->tasks[i].ready_flag) == 1) {
        ncompleted++;
      }
      _task_fifo->transmit_complete_flag = 1;
    }
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

void recv_task_dispatcher(void* buffer, size_t sz, RecvChannel* channels[], void* rings[], int n_channels) {
  // TODO: implement
}