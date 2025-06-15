#ifndef FUSELINK_CHANNEL_H
#define FUSELINK_CHANNEL_H

#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <cuda_runtime.h>
#include "task.h"
#include "debug.h"
#include "device.h"

#define N_CHANNELS 16
#define MAX_PEERS 16
#define CHANNEL_TYPE_SEND 0
#define CHANNEL_TYPE_RECV 1

/*
Channel is the basic unit for communication between two gpus across nodes.
Each channel has:
1. a task fifo for receiver to request a chunk from sender

data transmission
*/

#define TASK_FIFO_SZ 16

// Channel states
enum class ChannelState {
  IDLE,    // Channel is idle, waiting for new tasks
  WORKING, // Channel is processing tasks
  DONE     // Channel has finished processing all tasks
};

struct Task {
  int task_id;
  void* buffer;
  size_t buffer_size;  // Size of the buffer in bytes
  alignas(64) char padding[64];  // Ensure cache line alignment
};

struct TaskFifo {
  alignas(64) Task tasks[TASK_FIFO_SZ];
  volatile int size_fifo[TASK_FIFO_SZ];  // -1: empty, >0: valid task size
  int task_head; // read by CPU thread
  int task_tail; // write by GPU thread
  alignas(64) char padding[64];  // Ensure cache line alignment
};

class Channel {
public:
  Channel() {
    // Allocate TaskFifo in unified memory
    CUDA_CHECK(cudaMallocManaged(&_task_fifo, sizeof(CpuTaskFifo)));
    _task_fifo->head = 0;
    _task_fifo->tail = 0;
    // init the status as idle, so that it will pending and wait for work at the beginning.
    _state = ChannelState::IDLE;
    _total_tasks = 0;
    _processed_tasks = 0;
    _finished_tasks = 0;
  }

  virtual ~Channel() {
    if (_task_fifo) {
      CUDA_CHECK(cudaFree(_task_fifo));
    }
  }

  // Set number of tasks to process and wake up worker
  CpuTask* setTask(CpuTask &task) {
    std::unique_lock<std::mutex> lock(_mutex);
    CpuTask* result = nullptr;
    _total_tasks++;
    if (_task_fifo->head - _task_fifo->tail < FIFO_SZ) { 
      const int idx = _task_fifo->head % FIFO_SZ;
      _task_fifo->tasks[idx] = task;
      result = &_task_fifo->tasks[idx];
      __sync_synchronize();
      _task_fifo->head++;
    } else {
      // if the fifo is full, report failure
      printf("Channel fifo is full\n");
    }
    _state = ChannelState::WORKING;
    _cv.notify_one();
    return result;
  }

  // Get current state
  ChannelState getState() const {
    return _state;
  }

  int setDoneIfAllFinished() {
    // lock mutex and check if all tasks finished
    // if (_finished_tasks == _total_tasks) {
    std::unique_lock<std::mutex> lock(_mutex);
    _state = ChannelState::DONE;
    _cv.notify_one();
    return 1;
    // }
    // return 0;
  }

  // Get number of processed tasks
  int getProcessedTasks() const {
    return _processed_tasks;
  }

  // Get total number of tasks
  int getTotalTasks() const {
    return _total_tasks;
  }

  CpuTaskFifo* getTaskFifo() {
    return _task_fifo;  // Return unified memory pointer
  }

  // run until processed all tasks
  virtual void threadMain() = 0;

protected:
  // interactions with gpu
  // TaskFifo* _task_fifo;  // Unified memory pointer
  CpuTaskFifo* _task_fifo; // new
  ChannelState _state;   // Current channel state
  int _total_tasks;      // Total number of tasks to process
  int _processed_tasks;  // Number of tasks processed so far
  int _finished_tasks;   // ..
  std::condition_variable _cv;
  std::mutex _mutex;
};

class SendChannel: public Channel {
public:
  SendChannel();
  int checkSendingStatus();
  virtual ~SendChannel() override;
  void threadMain() override;
private:
  std::thread _thread;
};

class RecvChannel: public Channel {
public:
  RecvChannel();
  virtual ~RecvChannel() override;
  void threadMain() override;

private:
  std::thread _thread;
};

extern SendChannel* global_send_channels[MAX_PEERS][N_CHANNELS];
extern RecvChannel* global_recv_channels[MAX_PEERS][N_CHANNELS];

// void init_channels

void allocate_channels(int peer_id, int n_channels, void** channel_array, int channel_type);

/*
Task dispatcher thread, it manages how many tasks each channel should process.
buffer: the buffer to be sent from / sent to
sz: size
channels: pointer to the channels array
rings: pointer to the NIC rings array, same size as the channels
*/

void send_task_dispatcher(void* buffer, size_t sz, SendChannel* channels[], GpuTaskFifo fifos[], NicRing* rings[], int n_channels);
void recv_task_dispatcher(void* buffer, size_t sz, RecvChannel* channels[], void* rings[], int n_channels);
// no need to deallocate channels, the channels will be released after the transmission finishes.

#endif
