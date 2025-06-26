#include "channel.h"
#include "sendrecv.cuh"
#include <unistd.h>
#include "debug.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <infiniband/verbs.h>
#include <cstring>
#include <cstdio>

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
    while (_task_fifo->head > _task_fifo->tail ) { // have task to process
      // Get the task
      __sync_synchronize();
      const int idx = _task_fifo->tail % FIFO_SZ;
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

void recv_task_dispatcher(void* buffer, size_t sz, RecvChannel* channels[], void* rings[], int n_channels) {
  // TODO: implement
}