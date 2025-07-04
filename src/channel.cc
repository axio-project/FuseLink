#include "channel.h"
#include "sendrecv.cuh"
#include <unistd.h>
#include <vector>
#include <chrono>
<<<<<<< Updated upstream
#include "debug.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <infiniband/verbs.h>
#include <cstring>
#include <cstdio>
=======
>>>>>>> Stashed changes

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

int Channel::create_rdmaResources(struct ibv_device **dev_list, int dev_id) {
  ctx = ibv_open_device(dev_list[dev_id]);
  if (!ctx) {
    fprintf(stderr, "Failed to open RDMA device.\n");
    return -1;
  }
  pd = ibv_alloc_pd(ctx);
  if (!pd) {
    fprintf(stderr, "Failed to allocate protection domain.\n");
    ibv_close_device(ctx);
    return -1;
  }
  cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
  if (!cq) {
    fprintf(stderr, "Failed to create completion queue.\n");
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    return -1;
  }
  struct ibv_qp_init_attr qp_init_attr = {};
  qp_init_attr.send_cq = cq;
  qp_init_attr.recv_cq = cq;
  qp_init_attr.qp_type = IBV_QPT_RC;
  qp_init_attr.cap.max_send_wr = 10;
  qp_init_attr.cap.max_recv_wr = 10;
  qp_init_attr.cap.max_send_sge = 1;
  qp_init_attr.cap.max_recv_sge = 1;
  qp = ibv_create_qp(pd, &qp_init_attr);
  if (!qp) {
    fprintf(stderr, "Failed to create queue pair.\n");
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    return -1;
  }

  struct ibv_qp_attr qp_attr = {};
  qp_attr.qp_state = IBV_QPS_INIT;
  qp_attr.port_num = 1;
  qp_attr.pkey_index = 0;
  qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
  if (ibv_modify_qp(qp, &qp_attr,
                    IBV_QP_STATE | IBV_QP_PORT | IBV_QP_PKEY_INDEX |
                        IBV_QP_ACCESS_FLAGS)) {
    fprintf(stderr, "Failed to modify QP to INIT state.\n");
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    return -1;
  }
  return 0;
}

// SendChannel class implementation
SendChannel::SendChannel() : Channel() {
    // Base class constructor handles initialization
}

SendChannel::~SendChannel() {
    // Base class destructor handles cleanup
}

int SendChannel::create_rdmaResources(struct ibv_device **dev_list,
                                      int dev_id) {
  int ret = Channel::create_rdmaResources(dev_list, dev_id);
  if (ret < 0) {
    fprintf(stderr, "Failed to create RDMA resources for SendChannel.\n");
    return ret;
  }
  _send_comm.fifoHead = 0;
  _send_comm.fifoMr = ibv_reg_mr(
      pd, &_send_comm.fifo, sizeof(_send_comm.fifo),
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (!_send_comm.fifoMr) {
    fprintf(stderr, "Failed to register memory region for SendChannel.\n");
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    return -1;
  }
  return 0;
}

int SendChannel::Connect(int port) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  int ret = bind(sockfd, (struct sockaddr *)&addr, sizeof(addr));
  if (ret < 0) {
    perror("bind");
    close(sockfd);
    return -1;
  }
  ret = listen(sockfd, 1);
  if (ret < 0) {
    perror("listen");
    close(sockfd);
    return -1;
  }
  int connfd = accept(sockfd, NULL, NULL);

  struct {
    uint32_t qp_num;
    union ibv_gid gid;
    uint64_t fifo_addr;
    uint32_t rkey;
  } local, remote;

  if (ibv_query_gid(ctx, 1, 0, &local.gid)) {
    fprintf(stderr, "ibv_query_gid failed\n");
    close(connfd);
    close(sockfd);
    return -1;
  }
  local.qp_num = qp->qp_num;
  local.fifo_addr = (uint64_t)&_send_comm.fifo;
  local.rkey = _send_comm.fifoMr->rkey;

  write(connfd, &local, sizeof(local));
  read(connfd, &remote, sizeof(remote));

  DEBUG_PRINT("Local QP: num=%u, gid=%s, fifo_addr=%lu, rkey=%u\n",
              local.qp_num, inet_ntoa(*(struct in_addr *)&local.gid.global.subnet_prefix),
              local.fifo_addr, local.rkey);
  DEBUG_PRINT("Remote QP: num=%u, gid=%s, fifo_addr=%lu, rkey=%u\n",
              remote.qp_num, inet_ntoa(*(struct in_addr *)&remote.gid.global.subnet_prefix),
              remote.fifo_addr, remote.rkey);
  struct ibv_qp_attr attr = {};
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = IBV_MTU_1024;
  attr.rq_psn = 0;
  attr.dest_qp_num = remote.qp_num;
  attr.max_dest_rd_atomic = 1;
  attr.min_rnr_timer = 12;
  attr.ah_attr.is_global = 1;
  attr.ah_attr.dlid = 0;
  attr.ah_attr.port_num = 1;
  attr.ah_attr.grh.dgid = remote.gid;
  attr.ah_attr.grh.sgid_index = 0;
  attr.ah_attr.grh.hop_limit = 1;

  if (ibv_modify_qp(qp, &attr,
                    IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_RQ_PSN |
                        IBV_QP_DEST_QPN | IBV_QP_MAX_DEST_RD_ATOMIC |
                        IBV_QP_MIN_RNR_TIMER)) {
    perror("ibv_modify_qp");
    fprintf(stderr, "Failed to modify QP to RTR state.\n");
    close(connfd);
    close(sockfd);
    return -1;
  }

  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTS;
  attr.timeout = 14;
  attr.retry_cnt = 7;
  attr.rnr_retry = 7;
  attr.sq_psn = 0;
  attr.max_rd_atomic = 1;
  if (ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC)) {
        fprintf(stderr, "Failed to modify QP to RTS\n");
        exit(EXIT_FAILURE);
    } else {
        DEBUG_PRINT("QP state changed from RTR to RTS.\n");
    }
  return 0;
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

int RecvChannel::create_rdmaResources(struct ibv_device **dev_list,
                                      int dev_id) {
  int ret = Channel::create_rdmaResources(dev_list, dev_id);
  if (ret < 0) {
    fprintf(stderr, "Failed to create RDMA resources for RecvChannel.\n");
    return ret;
  }
  _rem_fifo.fifoTail = 0;
  _rem_fifo.mr = ibv_reg_mr(
      pd, &_rem_fifo.elems, sizeof(_rem_fifo.elems),
      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  if (!_rem_fifo.mr) {
    fprintf(stderr, "Failed to register memory region for RecvChannel.\n");
    ibv_destroy_qp(qp);
    ibv_destroy_cq(cq);
    ibv_dealloc_pd(pd);
    ibv_close_device(ctx);
    return -1;
  }
  return 0;
}

int RecvChannel::Connect(const char *peer_ip, int peer_port) {
  int sockfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd < 0) {
    perror("socket");
    return -1;
  }

  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(peer_port);
  inet_pton(AF_INET, peer_ip, &addr.sin_addr);

  if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    perror("connect");
    close(sockfd);
    return -1;
  }

  struct {
    uint32_t qp_num;
    union ibv_gid gid;
    uint64_t fifo_addr;
    uint32_t rkey;
  } local, remote;

  if (ibv_query_gid(ctx, 1, 0, &local.gid)) {
    fprintf(stderr, "ibv_query_gid failed\n");
    close(sockfd);
    return -1;
  }
  local.qp_num = qp->qp_num;

  read(sockfd, &remote, sizeof(remote));
  write(sockfd, &local, sizeof(local));
  DEBUG_PRINT("Local QP: num=%u, gid=%s, fifo_addr=%lu, rkey=%u\n",
              local.qp_num, inet_ntoa(*(struct in_addr *)&local.gid.global.subnet_prefix),
              local.fifo_addr, local.rkey);
  DEBUG_PRINT("Remote QP: num=%u, gid=%s, fifo_addr=%lu, rkey=%u\n",
              remote.qp_num, inet_ntoa(*(struct in_addr *)&remote.gid.global.subnet_prefix),
              remote.fifo_addr, remote.rkey);

  _rem_fifo.addr = remote.fifo_addr;
  _rem_fifo.rkey = remote.rkey;

  struct ibv_qp_attr attr = {};
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = IBV_MTU_1024;
  attr.rq_psn = 0;
  attr.dest_qp_num = remote.qp_num;
  attr.max_dest_rd_atomic = 1;
  attr.min_rnr_timer = 12;
  attr.ah_attr.is_global = 1;
  attr.ah_attr.dlid = 0;
  attr.ah_attr.port_num = 1;
  attr.ah_attr.grh.dgid = remote.gid;
  attr.ah_attr.grh.sgid_index = 0;
  attr.ah_attr.grh.hop_limit = 1;

  if (ibv_modify_qp(qp, &attr,
                    IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_RQ_PSN |
                        IBV_QP_DEST_QPN | IBV_QP_MAX_DEST_RD_ATOMIC |
                        IBV_QP_MIN_RNR_TIMER)) {
    perror("ibv_modify_qp");
    fprintf(stderr, "Failed to modify QP to RTR state.\n");
    close(sockfd);
    return -1;
  }

  memset(&attr, 0, sizeof(attr));
  attr.qp_state = IBV_QPS_RTS;
  attr.timeout = 14;
  attr.retry_cnt = 7;
  attr.rnr_retry = 7;
  attr.sq_psn = 0;
  attr.max_rd_atomic = 1;
  if (ibv_modify_qp(qp, &attr,
                    IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT |
                        IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN |
                        IBV_QP_MAX_QP_RD_ATOMIC)) {
    fprintf(stderr, "Failed to modify QP to RTS\n");
    exit(EXIT_FAILURE);
  } else {
    DEBUG_PRINT("QP state changed from RTR to RTS.\n");
  }
  return 0;
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
    printf("write to fifo %d, head %d, mem ptr %p\n", gpu_idx, fifos[gpu_idx].head, (void*)mem.device_ptr);
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

  bool check_flag = false;
  for (int i = 0; i < nchunks;) {
    // find an available channel
    int target_channel_idx = i % n_channels;
    // todo: select ring idx from recv msgs
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
            // read target ring and slot
            int target_ring_idx = fifos[i].tasks[j % FIFO_SZ].ring_id;
            int target_ring_slot = fifos[i].tasks[j % FIFO_SZ].buffer_slot;
            channels[target_ring_idx]->postSend(fifos[i].tasks[j % FIFO_SZ], j);
            channel_pending_tasks[i].push_back(j);
            sent_tasks++;
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
        uint64_t finished_tasks[8];
        int nfinished = 0;
        channels[i]->checkFinish(finished_tasks, &nfinished);
        for (int j = 0; j < nfinished; j++) {
          // free the ring slot
          auto finished_task_id = finished_tasks[j];
          deallocRingSlot(rings[fifos[i].tasks[finished_task_id % FIFO_SZ].ring_id], fifos[i].tasks[finished_task_id % FIFO_SZ].buffer_slot);
          channel_pending_tasks[i].erase(channel_pending_tasks[i].begin() + finished_task_id);
          pending_tasks--;
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
          channels[i]->postSend(task, j);
          channel_pending_tasks[i].push_back(j);
          sent_tasks++;
        }
      } // tailcache < tail
      tail_cache[i] = tail;
    }
  }

  while (pending_tasks > 0) {
    for (int i = 0; i < n_channels; i++) {
      uint64_t finished_tasks[8];
      int nfinished = 0;
      channels[i]->checkFinish(finished_tasks, &nfinished);
      for (int j = 0; j < nfinished; j++) {
        deallocRingSlot(rings[fifos[i].tasks[finished_tasks[j] % FIFO_SZ].ring_id], fifos[i].tasks[finished_tasks[j] % FIFO_SZ].buffer_slot);
        channel_pending_tasks[i].erase(channel_pending_tasks[i].begin() + finished_tasks[j]);
        pending_tasks--;
      }
    }// iterate through all channels
  }

}

void SendChannel::postSend(GeneralTask &task, int task_id) {
  RoceSend(task.buffer, task.buffer_size, _send_comm.fifoMr, qp, &_send_comm, task_id);
}

void SendChannel::checkFinish(size_t* finished_tasks, int* n) {
  RoceCheckFinish(finished_tasks, n, cq);
}

void recv_task_dispatcher(void* buffer, size_t sz, SendChannel* channels[], GeneralTaskFifo fifos[], NicRing* rings[], int n_channels, bool verify) {
  // TODO: implement
  const int batch_sz = 8;
  const int nchunks = CEIL_DIV(sz, FUSELINK_CHUNK_SZ);

  int pending_tasks = 0;
  int sent_tasks = 0;
  int tail_cache[N_CHANNELS] = {0};
  std::vector<int> channel_pending_tasks[N_CHANNELS];

  bool check_flag = false;
  for (int i = 0; i < nchunks;) {
    // find an available channel
    int target_channel_idx = i % n_channels;
    // todo: select ring idx from recv msgs
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

      // set general task, pending for recv
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
            // read target ring and slot
            int target_ring_idx = fifos[i].tasks[j % FIFO_SZ].ring_id;
            int target_ring_slot = fifos[i].tasks[j % FIFO_SZ].buffer_slot;
            channels[target_ring_idx]->postSend(fifos[i].tasks[j % FIFO_SZ], j);
            channel_pending_tasks[i].push_back(j);
            sent_tasks++;
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
        uint64_t finished_tasks[8];
        int nfinished = 0;
        channels[i]->checkFinish(finished_tasks, &nfinished);
        for (int j = 0; j < nfinished; j++) {
          // free the ring slot
          auto finished_task_id = finished_tasks[j];
          deallocRingSlot(rings[fifos[i].tasks[finished_task_id % FIFO_SZ].ring_id], fifos[i].tasks[finished_task_id % FIFO_SZ].buffer_slot);
          channel_pending_tasks[i].erase(channel_pending_tasks[i].begin() + finished_task_id);
          pending_tasks--;
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
          channels[i]->postSend(task, j);
          channel_pending_tasks[i].push_back(j);
          sent_tasks++;
        }
      } // tailcache < tail
      tail_cache[i] = tail;
    }
  }

  while (pending_tasks > 0) {
    for (int i = 0; i < n_channels; i++) {
      uint64_t finished_tasks[8];
      int nfinished = 0;
      channels[i]->checkFinish(finished_tasks, &nfinished);
      for (int j = 0; j < nfinished; j++) {
        deallocRingSlot(rings[fifos[i].tasks[finished_tasks[j] % FIFO_SZ].ring_id], fifos[i].tasks[finished_tasks[j] % FIFO_SZ].buffer_slot);
        channel_pending_tasks[i].erase(channel_pending_tasks[i].begin() + finished_tasks[j]);
        pending_tasks--;
      }
    }// iterate through all channels
  }
}