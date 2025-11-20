#ifndef FUSELINK_H
#define FUSELINK_H
#include "ibvwrap.h"
#include <vector>
#include <map>
#include "monitor.h"
#include "checks.h"
#include <assert.h>

#include "nccl_net.h" // interaction with nccl net plugin

extern ncclNet_t ncclNetPlugin_v7;

#define MAX_GPU_NUM 8

#define FUSELINK_STEPS 8

#define UPDATE_HACKING_THRESHOLD (FUSELINK_STEPS * sizeof(void*))



struct FuseLinkGidInfo {
  uint8_t link_layer;
  union ibv_gid localGid;
  union ibv_gid remoteGid;
};

class FuseLinkComm {
public:
  FuseLinkComm(int dev) {
    dev_ = dev;
  }
  int dev_;
  struct ibv_qp* qp;
  struct ibv_pd* pd;
  struct ibv_cq* cq;
  FuseLinkGidInfo gidInfo;

  enum CommStatus {
    RecvCommStatusInit = 0,
    RecvCommStatusListening,
    RecvCommStatusExchange,
    RecvCommStatusReady
  };
  CommStatus status_;
};

struct ncclProxySubArgsForHacking {
  void* connection;
  int channelId;
  int nsteps;
  ssize_t nbytes;
  int peer;

  int groupSize; // Number of consecutive sub operations sharing the same recvComm
  uint64_t base;
  uint64_t posted;
  uint64_t received;
  uint64_t flushed;
  uint64_t transmitted;
  uint64_t done;
  uint64_t end;
  void* requests[FUSELINK_STEPS];
  void* profilingEvents[FUSELINK_STEPS];
  void* recvRequestsCache[FUSELINK_STEPS];
  int recvRequestsSubCount;
}; // use by nccl, import this to hack sub->posted, sub->received, sub->transmitted, sub->done from nccl

struct FuseLinkMemRegion {
  uintptr_t start_addr;
  int dev; // mapped to which gpu, -1 if cpu
  int nic_dev; // mapped to which nic
  uint32_t sz; // 32bit is enough
  struct ibv_mr *mr[MAX_NIC_NUM + 1]; // registration on all NICs, with cpu mem on MAX_GPU_NUM index
  CUdeviceptr addr[MAX_NIC_NUM + 1]; // use this addr to enable RDMA access, cast to void* for CPU memory access
  CUmemGenericAllocationHandle hdl[MAX_NIC_NUM]; // physical memory, bind to addrs, dynamically bind to the addr
  int nrefs; // when 0, can be freed
  int nic2gpu[MAX_NIC_NUM]; // NIC index to GPU index, -1 if not mapped
  pthread_mutex_t lock; // update nrefs
};

typedef std::map<void*, FuseLinkMemRegion*> Addr2FuseLinkMemRegion;

enum FuseLinkConnSetupState {
  FuseLinkConnSetupStateInit = 0,
  FuseLinkConnSetupStatePending = 1,
  FuseLinkConnSetupStateReady = 2
};

class FuseLinkConnManager {
public:
  FuseLinkConnManager(int ndevs, int priority_dev) {
    ndevs_ = ndevs;
    priority_dev_ = priority_dev;
  }
  FuseLinkConnManager(int ndevs, int priority_dev, int nchannels) {
    ndevs_ = ndevs;
    priority_dev_ = priority_dev;
    nchannels_ = nchannels;
    for (int i = 0; i < nchannels; ++i) {
      txchannel2dev_[i] = priority_dev_;
      rxchannel2dev_[i] = priority_dev_;
    }
  }
  int GetNdevs() {
    return ndevs_;
  }
  ~FuseLinkConnManager() {
    // tear down all connections and monitor
    for (int i = 0; i < ndevs_; i++) {
      if (tx_send_comms_[i] != NULL) {
        ncclNetPlugin_v7.closeSend(tx_send_comms_[i]);
      }
      if (rx_recv_comms_[i] != NULL) {
        ncclNetPlugin_v7.closeRecv(rx_recv_comms_[i]);
      }
    }
    // delete monitor
    delete monitor_client_;
  }
  void SetUpMonitor() {
    monitor_client_ = new MonitorClient();
  }
  // FuseLinkConnection* GetAssignedDev(int channelId) {
  //   // return with assigned dev
  // }

  // FuseLinkConnection* ConnTx(int i) {
  //   return &tx_connections_[i];
  // }

  // FuseLinkConnection* ConnRx(int i) {
  //   return &rx_connections_[i];
  // }

  void PushTxSendComm(void* send_comm) {
    tx_send_comms_.push_back(send_comm);
  }

  int GetTxNum() {
    return tx_send_comms_.size();
  }
  
  void PushRxRecvComm(void* recv_comm) {
    rx_recv_comms_.push_back(recv_comm);
  }
  
  int GetRxNum() {
    return rx_recv_comms_.size();
  }

  bool MarkTxChannelActive(int channelId) {
    return MarkNicTxBusy(channel2dev(channelId), channelId);
  }

  bool MarkRxChannelActive(int channelId) {
    return MarkNicRxBusy(channel2dev(channelId), channelId);
  }

  void* RefreshTxComm(int channelId) {
    /*
      this function is called by the channel to get a NIC for tx
      each channel independently call this function, so this function should be implemented with thread safety
    */
    // if (channelId / 2 == 0) {
    //   // main channel
    //   return NULL;
    // } else {
    //   // side channel
    //   return tx_send_comms_[channel2dev(channelId)];
    // }
    return NULL;
  }

  void* bidTxChannel(int channelId, int txNicId) {
    // DEBUG: return NULL to enforce the use of comm itself
    return tx_send_comms_[txNicId];
    // DEBUG: end
    int group_id = txNicId / NNIC_PER_GROUP;
    int offset = txNicId % NNIC_PER_GROUP;
    return tx_send_comms_[txNicId];
    // return monitor_client_->bidTxChannel(group_id, offset);
  }

  bool RefreshRxComm(int channelId) {
    /* same as the tx version */
    if (channelId / 2 == 0) {
      return NULL;
    } else {
      return rx_recv_comms_[channel2dev(channelId)];
    }
  }

  int RegisterMem(void* ptr, size_t size, void** handle) {
    /*
      register the memory on all devices, we need an array of mr, use on demand
    */
    // not implemented yet
    return -1;
  }

  void* channel2txcomm(int channelId) {
    int dev = channel2dev(channelId);
    return tx_send_comms_[dev];
  }

  void* channel2rxcomm(int channelId) {
    int dev = channel2dev(channelId);
    return rx_recv_comms_[dev];
  }

  void* refreshTxComm(int channelId) {
    // call monitorclient to get a new nic
    int nic_id = txchannel2dev_[channelId];
    int group_id = nic_id / NNIC_PER_GROUP;
    int offset = nic_id % NNIC_PER_GROUP;
    // todo: update idle nic tx arguments.
    int dev = monitor_client_->getIdleNicTx(group_id, priority_dev_);
    txchannel2dev_[channelId] = dev;
    return tx_send_comms_[dev];
  }

  void* refreshRxComm(int channelId) {
    // call monitorclient to get a new nic
    int group_id = channelId / NNIC_PER_GROUP;
    int offset = channelId % NNIC_PER_GROUP;
    int dev = monitor_client_->getIdleNicRx(group_id, priority_dev_, 0xffffffff);
    rxchannel2dev_[channelId] = dev;
    return rx_recv_comms_[dev];
  }

  void* refreshRxComm(int channelId, int* txAvailable, int nNics) {
    // call monitorclient to get a new nic
    // convert txAvailable to mask

    // DEBUG: return NULL to enforce the use of the comm itself
    return NULL;
    // DEBUG: end

    int mask = 0;
    for (int i = 0; i < nNics; ++i) {
      if (txAvailable[i]) {
        mask |= (1 << i);
      }
    }
    int group_id = rxchannel2dev_[channelId] / NNIC_PER_GROUP;
    int dev = monitor_client_->getIdleNicRx(group_id, priority_dev_, mask);
    rxchannel2dev_[channelId] = dev;
    return rx_recv_comms_[dev];
  }

  void* refreshRxComm(int channelId, uint32_t txAvailable, int nNics, int* fuselink_offset) {
    // call monitorclient to get a new nic
    // convert txAvailable to mask

    // strategy for testing packet spraying in network
    int dev = channelId % nNics;
    *fuselink_offset = dev;
    return rx_recv_comms_[dev];

    // int group_id = channelId / NNIC_PER_GROUP;
    // int dev;
    // dev = monitor_client_->getIdleNicRx(group_id, priority_dev_, txAvailable);
    // rxchannel2dev_[channelId] = dev;
    // return rx_recv_comms_[dev];
  }


  void updateTxChannelActive(int channelId) { // called by the channel
    int dev = channel2dev(channelId);
    int group_id = dev / NNIC_PER_GROUP;
    int offset = dev % NNIC_PER_GROUP;
    monitor_client_->markNicTxAsActive(group_id, offset);
  }

  void updateRxChannelActive(int channelId) {
    int dev = channel2dev(channelId);
    int group_id = dev / NNIC_PER_GROUP;
    int offset = dev % NNIC_PER_GROUP;
    monitor_client_->markNicRxAsActive(group_id, offset);
  }

  uint32_t getTxUsage() {
    return 0xffffffff; // all available, place holder.
  }

  uint32_t refreshTxUsage() {
    return 0xffffffff; // all available, place holder.
  }

  FuseLinkConnSetupState tx_setup_state_;
  FuseLinkConnSetupState rx_setup_state_;
  std::vector<void*> tx_send_comms_; // actually tx channels
  std::vector<void*> rx_recv_comms_; // actually rx channels
private:

  bool MarkNicTxBusy(int dev, int channelId) {
    /*
      mark the nic busy after allocating the NIC to the channel.
      the allocation should be exclusive.
      This function should be called on two conditions:
      1. the primary NIC is occupied by the main channel
      2. occupy the NIC for side channel
      todo: this function should be called internally by the fuselink, not by other objects, private function
    */
    /*
    occupy through the interface of monitor
    */
    int group_id = dev / NNIC_PER_GROUP;
    int offset = dev % NNIC_PER_GROUP;
    monitor_client_->markNicTxAsActive(group_id, offset);
    return true;
  }

  bool MarkNicRxBusy(int dev, int channelId) {
    /* same as the tx version */
    int group_id = dev / NNIC_PER_GROUP;
    int offset = dev % NNIC_PER_GROUP;
    monitor_client_->markNicRxAsActive(group_id, offset);
    return true;
  }

  int channel2dev(int channelId) {
    return txchannel2dev_[channelId];
  }

  int ndevs_;
  int priority_dev_;
  // std::vector<FuseLinkConnection> tx_connections_;
  // std::vector<FuseLinkConnection> rx_connections_;
  int nchannels_;

  MonitorClient *monitor_client_;
  std::map<int, int> txchannel2dev_; // dev to comm.
  std::map<int, int> rxchannel2dev_; // dev to comm.
};

static inline void FuseLinkMemRegionInit(int nGPUs, int nNics, int* NIC2GPU, void* base_addr, size_t size, \
   int src_dev, int target_nic_dev, FuseLinkMemRegion *flmr, bool is_cuda_mem) {
  flmr->nrefs = 1;
  flmr->dev = src_dev;
  flmr->nic_dev = target_nic_dev;
  // copy nic2gpu to flmr
  for (int i = 0; i < nNics; i++) {
    flmr->nic2gpu[i] = NIC2GPU[i];
  }
  for (int i = nNics; i < MAX_NIC_NUM; i++) {
    flmr->nic2gpu[i] = -1;
  }
  pthread_mutex_init(&flmr->lock, NULL);

  if (!is_cuda_mem) {
    // allocate cpu memory
    flmr->start_addr = (uintptr_t) base_addr;
    flmr->dev = -1;
    flmr->nic_dev = target_nic_dev;
    flmr->addr[MAX_GPU_NUM] = (CUdeviceptr) base_addr;
    flmr->mr[MAX_GPU_NUM] = nullptr;
    // cut the lower bits
    flmr->sz = static_cast<uint32_t>(size);
    return;
  }

  size_t granularity = 0;
  size_t aligned_sz = 0;
  for (int i = 0; i < nNics; i++) {
    CUmemAllocationProp prop = {};
    prop.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    prop.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    assert(NIC2GPU[i] >= 0 && NIC2GPU[i] < MAX_GPU_NUM);
    prop.location.id = NIC2GPU[i];
    prop.requestedHandleTypes = CU_MEM_HANDLE_TYPE_POSIX_FILE_DESCRIPTOR;
    prop.allocFlags.gpuDirectRDMACapable = 1;
    

    FL_CUCHECK(cuMemGetAllocationGranularity(&granularity, &prop, CU_MEM_ALLOC_GRANULARITY_MINIMUM));
    aligned_sz = ((size + granularity - 1) / granularity) * granularity;
    
    FL_CUCHECK(cuMemCreate(&flmr->hdl[i], aligned_sz, &prop, 0));
    
    // create address range
    FL_CUCHECK(cuMemAddressReserve(&flmr->addr[i], aligned_sz, 0, 0, 0));
    // map address to the physical memory
    FL_CUCHECK(cuMemMap(flmr->addr[i], aligned_sz, 0, flmr->hdl[i], 0));
    // set the mr to null, wait for registration
    flmr->mr[i] = NULL;
    CUmemAccessDesc accessDesc = {};
    accessDesc.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    accessDesc.location.id = src_dev; // enable access from the source gpu
    accessDesc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    FL_CUCHECK(cuMemSetAccess(flmr->addr[i], aligned_sz, &accessDesc, 1));

  }
  
  CUmemGenericAllocationHandle origin_hdl;
  FL_CUCHECK(cuMemRetainAllocationHandle(&origin_hdl, base_addr));
  FL_CUCHECK(cuMemUnmap((CUdeviceptr) base_addr, aligned_sz));
  // get the original handle
  FL_CUCHECK(cuMemRelease(origin_hdl));
  
  // printf("map buffer from gpu %d to target gpu %d for nic %d\n", src_dev, NIC2GPU[target_nic_dev], target_nic_dev);
  FL_CUCHECK(cuMemMap((CUdeviceptr) base_addr, aligned_sz, 0, flmr->hdl[target_nic_dev], 0));
  
  // set access to base_addr
  CUmemAccessDesc accessDesc = {};
  accessDesc.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
  accessDesc.location.id = src_dev; // enable access from the source gpu
  accessDesc.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
  FL_CUCHECK(cuMemSetAccess((CUdeviceptr) base_addr, aligned_sz, &accessDesc, 1));
  
  flmr->start_addr = (uintptr_t) base_addr;
  flmr->dev = NIC2GPU[target_nic_dev];
  flmr->nic_dev = target_nic_dev;
  flmr->sz = aligned_sz;

  return;
}

void FuseLinkMemRegionDestroy(FuseLinkMemRegion *flmr);

void FuseLinkMemRegionRemap(FuseLinkMemRegion *flmr, int target_dev);

#endif

