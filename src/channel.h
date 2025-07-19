#ifndef FUSELINK_CHANNEL_H
#define FUSELINK_CHANNEL_H

#include <algorithm>
#include <infiniband/verbs.h>
#include "net.h"
#include <thread>
#include <condition_variable>
#include <mutex>
#include <cuda_runtime.h>
#include "task.h"
#include "debug.h"
#include "device.h"
#include "net.h"

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
    lock.unlock();
    _cv.notify_one();
    return result;
  }

  GeneralTask* setTask_general(GeneralTask &task) {
    std::unique_lock<std::mutex> lock(_mutex);
    GeneralTask* result = nullptr;
    int write_idx = _general_task_fifo->head;
    int read_idx = _general_task_fifo->tail;
    if (write_idx - read_idx < FIFO_SZ) {
      const int idx = write_idx % FIFO_SZ;
      _general_task_fifo->tasks[idx] = task;
      __sync_synchronize();  // tasks must update before head
      _general_task_fifo->head = write_idx + 1;
      result = &_general_task_fifo->tasks[idx];
    } else {
      printf("General channel fifo is full\n");
      lock.unlock();
      return nullptr;
    }
    _total_tasks++;
    _state = ChannelState::WORKING;
    lock.unlock();
    _cv.notify_one();
    return result;
  }

  // Get current state
  ChannelState getState() const {
    return _state;
  }

  int setDoneIfAllFinished() {
    std::unique_lock<std::mutex> lock(_mutex);
    _state = ChannelState::DONE;
    lock.unlock();
    _cv.notify_one();
    return 1;
  }

  int updateTaskFifoTail() {
    auto read_idx = _general_task_fifo->tail;
    auto write_idx = _general_task_fifo->head;
    while (_general_task_fifo->tasks[read_idx].stage == TASK_STAGE_FINISH && read_idx < write_idx) {
      read_idx++;
    }
    _general_task_fifo->tail = read_idx;
    __sync_synchronize();
    return read_idx;
  }

  int setDoneIfAllFinished_general() {
    auto read_idx = _general_task_fifo->tail;
    auto write_idx = _general_task_fifo->head;
    while (_general_task_fifo->tasks[read_idx].stage == TASK_STAGE_FINISH && read_idx < write_idx) {
      read_idx++;
    }
    _general_task_fifo->tail = read_idx;
    __sync_synchronize();
    if (read_idx == write_idx) {
      // printf("Channel is done\n");
      std::unique_lock<std::mutex> lock(_mutex);
      _state = ChannelState::DONE;
      lock.unlock();
      _cv.notify_one();
      return 1;
    }
    return 0;
  }

  void setGeneralTaskFifo(GeneralTaskFifo* fifo) {
    _general_task_fifo = fifo;
  }

  int create_rdmaResources(struct ibv_device **dev_list, int dev_id);

  struct ibv_mr* reg_mr(void* addr, size_t size, int access_flags) {
    // Register memory region for RDMA
    struct ibv_mr* mr = ibv_reg_mr(pd, addr, size, access_flags);
    if (!mr) {
      fprintf(stderr, "Failed to register memory region.\n");
      return nullptr;
    }
    return mr;
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
  virtual void threadMain_general() = 0;

protected:
  // interactions with gpu
  GeneralTaskFifo* _general_task_fifo; // in unified memory, accessed by gpu and cpu
  CpuTaskFifo* _task_fifo; // new
  ChannelState _state;   // Current channel state
  int _total_tasks;      // Total number of tasks to process
  int _processed_tasks;  // Number of tasks processed so far
  int _finished_tasks;   // ..
  ibv_context* ctx = nullptr; // RDMA context
  ibv_pd* pd = nullptr; // Protection domain
  ibv_cq* cq = nullptr; // Completion queue
  ibv_qp* qp = nullptr; // Queue pair
  std::condition_variable _cv;
  std::mutex _mutex; // use cv and mutex to avoid infinite loop of cpu threads
};

class SendChannel: public Channel {
public:
  SendChannel();
  int checkSendingStatus();
  int create_rdmaResources(struct ibv_device **dev_list, int dev_id);
  int Connect(int port);
  virtual ~SendChannel() override;
  void threadMain() override;
  void threadMain_general() override;
  // post write to the remote host, with wr_id being task_id and imm_data being workload info
  void postSend(GeneralTask &task, int task_id);
  void checkFinish(size_t* finished_tasks, int* n);
private:
  std::thread _thread;
  struct RoceSendComm _send_comm;
};

class RecvChannel: public Channel {
public:
  RecvChannel();
  int Connect(const char* peer_ip, int peer_port);
  int create_rdmaResources(struct ibv_device **dev_list, int dev_id);
  virtual ~RecvChannel() override;
  void threadMain() override;
  void threadMain_general() override;
  GeneralTask* setTask_general(GeneralTask &task) {
    std::unique_lock<std::mutex> lock(_mutex);
    GeneralTask* result = nullptr;
    int write_idx = _general_task_fifo->head;
    // write to the head by CPU, read from the tail by GPU
    int read_idx = _general_task_fifo->tail;
    if (write_idx - read_idx < FIFO_SZ) {
      const int idx = write_idx % FIFO_SZ;
      _general_task_fifo->tasks[idx] = task;
      __sync_synchronize();  // tasks must update before head
      _general_task_fifo->head = write_idx + 1;
      result = &_general_task_fifo->tasks[idx];
      // wait for GPU to fetch the data to the final destination
    } else {
      printf("General channel fifo is full\n");
      lock.unlock();
      return nullptr;
    }
    _total_tasks++;
    _state = ChannelState::WORKING;
    lock.unlock();
    _cv.notify_one();
    return result;
  }

  // post receive for the receive host, with wr_id being task_id and fifo
  void postRecvRequest(GeneralTask &task, int task_id);

private:
  std::thread _thread;
  struct RemFifo _rem_fifo;
};

extern SendChannel* global_send_channels[MAX_PEERS][N_CHANNELS];
extern RecvChannel* global_recv_channels[MAX_PEERS][N_CHANNELS];

void allocate_channels(int peer_id, int n_channels, void** channel_array, int channel_type);

/*
Task dispatcher thread, it manages how many tasks each channel should process.
buffer: the buffer to be sent from / sent to
sz: size
channels: pointer to the channels array
rings: pointer to the NIC rings array, same size as the channels
*/

void send_task_dispatcher(void* buffer, size_t sz, SendChannel* channels[], GpuTaskFifo fifos[], NicRing* rings[], int n_channels);
void send_task_dispatcher_general(void* buffer, size_t sz, SendChannel* channels[], GeneralTaskFifo fifos[], NicRing* rings[], int n_channels, bool verify=false);
void recv_task_dispatcher(void* buffer, size_t sz, SendChannel* channels[], GeneralTaskFifo fifos[], NicRing* rings[], int n_channels, bool verify);
// no need to deallocate channels, the channels will be released after the transmission finishes.

#endif
