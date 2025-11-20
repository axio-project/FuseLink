/*************************************************************************
 * Copyright (c) 2016-2022, NVIDIA CORPORATION. All rights reserved.
 *
 * See LICENSE.txt for license information
 ************************************************************************/

#include "nccl.h"
#include "core.h"
#include "socket.h"
#include "net.h"
#include "graph.h"
#include "utils.h"
#include "param.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/types.h>
#include <unistd.h>
#define ENABLE_TIMER 0
#include "timer.h"

#include "ibvwrap.h"

#include "cudawrap.h"

// #include "cumem.h"
#include "fuselink.h"

FuseLinkConnManager* fuselink_conn_manager = nullptr;
pthread_mutex_t addr2flmr_lock = PTHREAD_MUTEX_INITIALIZER;
Addr2FuseLinkMemRegion addr2flmr;

#define MAXNAMESIZE 64
#define N_FINISHED_BATCH 8
static char ncclIbIfName[MAX_IF_NAME_SIZE+1];
static union ncclSocketAddress ncclIbIfAddr;

struct ncclIbMr {
  uintptr_t addr;
  int pages;
  int refs;
  ibv_mr *mr;
};

struct ncclIbMrCache {
  struct ncclIbMr *slots;
  int capacity, population;
};
#define FUSELINK_MAX_NCHANNEL 16

static int ncclNIbDevs = -1;
static int NGPUs = -1;
int NIC2GPU[MAX_NIC_NUM]; // NIC index to GPU index, -1 if not mapped
int Channel2NIC[FUSELINK_MAX_NCHANNEL]; // Channel index to NIC index, -1 if not mapped.
// specified by mapping, 0,1,2,3 meaning NIC 0, 1, 2, 3 mapped to GPU 0, 1, 2, 3


/* Every channel connects to all NICs */

FuseLinkComm* fuselink_send_comm_[FUSELINK_MAX_NCHANNEL][MAX_NIC_NUM]; // initialized as nullptr
FuseLinkComm* fuselink_recv_comm_[FUSELINK_MAX_NCHANNEL][MAX_NIC_NUM];


struct alignas(64) ncclIbDev {
  pthread_mutex_t lock;
  int device;
  uint64_t guid;
  uint8_t port;
  uint8_t link;
  int speed;
  ibv_context* context;
  int pdRefs;
  ibv_pd* pd;
  char devName[MAXNAMESIZE];
  char* pciPath;
  int realPort;
  int maxQp;
  struct ncclIbMrCache mrCache;
  int ar; // ADAPTIVE_ROUTING
};

#define MAX_IB_PORT 15
struct userIbDev {
  char devName[MAXNAMESIZE];
  uint16_t port_en;
};

#define MAX_IB_DEVS 32
struct ncclIbDev ncclIbDevs[MAX_IB_DEVS];
struct userIbDev userIbDevs[MAX_IB_DEVS];
pthread_mutex_t ncclIbLock = PTHREAD_MUTEX_INITIALIZER;
static int ncclIbRelaxedOrderingEnabled = 0;

NCCL_PARAM(IbGidIndex, "IB_GID_INDEX", 0);
NCCL_PARAM(IbTimeout, "IB_TIMEOUT", 18);
NCCL_PARAM(IbRetryCnt, "IB_RETRY_CNT", 7);
NCCL_PARAM(IbPkey, "IB_PKEY", 0);
NCCL_PARAM(IbUseInline, "IB_USE_INLINE", 0);
NCCL_PARAM(IbSl, "IB_SL", 0);
NCCL_PARAM(IbTc, "IB_TC", 0);
NCCL_PARAM(IbArThreshold, "IB_AR_THRESHOLD", 8192);
NCCL_PARAM(IbPciRelaxedOrdering, "IB_PCI_RELAXED_ORDERING", 2);
NCCL_PARAM(IbAdaptiveRouting, "IB_ADAPTIVE_ROUTING", -2);
NCCL_PARAM(BuffSize, "BUFFSIZE", -2);


pthread_t ncclIbAsyncThread;
static void* ncclIbAsyncThreadMain(void* args) {
  struct ibv_context* context = (struct ibv_context*)args;
  while (1) {
    struct ibv_async_event event;
    if (ncclSuccess != wrap_ibv_get_async_event(context, &event)) { break; }
    char *str;
    if (ncclSuccess != wrap_ibv_event_type_str(&str, event.event_type)) { break; }
    if (event.event_type != IBV_EVENT_COMM_EST)
      WARN("NET/FuseLink : Got async event : %s", str);
    if (ncclSuccess != wrap_ibv_ack_async_event(&event)) { break; }
  }
  return NULL;
}

NCCL_PARAM(IbDisable, "IB_DISABLE", 0);
NCCL_PARAM(IbMergeVfs, "IB_MERGE_VFS", 1);
NCCL_PARAM(FuseLinkPriorityDev, "FUSELINK_PRIORITY_DEV", 0);

static ncclResult_t ncclIbGetPciPath(char* devName, char** path, int* realPort) {
  char devicePath[PATH_MAX];
  snprintf(devicePath, PATH_MAX, "/sys/class/infiniband/%s/device", devName);
  char* p = realpath(devicePath, NULL);
  if (p == NULL) {
    WARN("Could not find real path of %s (%s)", devName, devicePath);
  } else {
    // Merge multi-port NICs into the same PCI device
    p[strlen(p)-1] = '0';
    // Also merge virtual functions (VF) into the same device
    if (ncclParamIbMergeVfs()) p[strlen(p)-3] = p[strlen(p)-4] = '0';
    // And keep the real port aside (the ibv port is always 1 on recent cards)
    *realPort = 0;
    for (int d=0; d<ncclNIbDevs; d++) {
      if (strcmp(p, ncclIbDevs[d].pciPath) == 0) (*realPort)++;
    }
  }
  *path = p;
  return ncclSuccess;
}

static int ibvWidths[] = { 1, 4, 8, 12, 2 };
static int ibvSpeeds[] = {
  2500,  /* SDR */
  5000,  /* DDR */
  10000, /* QDR */
  10000, /* QDR */
  14000, /* FDR */
  25000, /* EDR */
  50000, /* HDR */
  100000 /* NDR */ };

static int firstBitSet(int val, int max) {
  int i = 0;
  while (i<max && ((val & (1<<i)) == 0)) i++;
  return i;
}
static int ncclIbWidth(int width) {
  return ibvWidths[firstBitSet(width, sizeof(ibvWidths)/sizeof(int)-1)];
}
static int ncclIbSpeed(int speed) {
  return ibvSpeeds[firstBitSet(speed, sizeof(ibvSpeeds)/sizeof(int)-1)];
}

// Determine whether RELAXED_ORDERING is enabled and possible
static int ncclIbRelaxedOrderingCapable(void) {
  int roMode = ncclParamIbPciRelaxedOrdering();
  ncclResult_t r = ncclInternalError;
  if (roMode == 1 || roMode == 2) {
    // Query IBVERBS_1.8 API - needed for IBV_ACCESS_RELAXED_ORDERING support
    r = wrap_ibv_reg_mr_iova2(NULL, NULL, NULL, 0, 0, 0);
  }
  return r == ncclInternalError ? 0 : 1;
}

ncclResult_t ncclIbInit(ncclDebugLogger_t logFunction) {
  if (ncclParamIbDisable()) return ncclInternalError;
  static int shownIbHcaEnv = 0;
  if(wrap_ibv_symbols() != ncclSuccess) { return ncclInternalError; }

  if (ncclNIbDevs == -1) {
    pthread_mutex_lock(&ncclIbLock);
    wrap_ibv_fork_init();
    if (ncclNIbDevs == -1) {
      ncclNIbDevs = 0;
      if (ncclFindInterfaces(ncclIbIfName, &ncclIbIfAddr, MAX_IF_NAME_SIZE, 1) != 1) {
        WARN("NET/FuseLink : No IP interface found.");
        return ncclInternalError;
      }

      // Detect IB cards
      int nIbDevs;
      struct ibv_device** devices;

      // Check if user defined which IB device:port to use
      char* userIbEnv = getenv("NCCL_IB_HCA");
      if (userIbEnv != NULL && shownIbHcaEnv++ == 0) INFO(NCCL_NET|NCCL_ENV, "NCCL_IB_HCA set to %s", userIbEnv);
      struct netIf userIfs[MAX_IB_DEVS];
      bool searchNot = userIbEnv && userIbEnv[0] == '^';
      if (searchNot) userIbEnv++;
      bool searchExact = userIbEnv && userIbEnv[0] == '=';
      if (searchExact) userIbEnv++;
      int nUserIfs = parseStringList(userIbEnv, userIfs, MAX_IB_DEVS);

      if (ncclSuccess != wrap_ibv_get_device_list(&devices, &nIbDevs)) return ncclInternalError;

      for (int d=0; d<nIbDevs && ncclNIbDevs<MAX_IB_DEVS; d++) {
        struct ibv_context * context;
        if (ncclSuccess != wrap_ibv_open_device(&context, devices[d]) || context == NULL) {
          WARN("NET/FuseLink : Unable to open device %s", devices[d]->name);
          continue;
        }
        int nPorts = 0;
        struct ibv_device_attr devAttr;
        memset(&devAttr, 0, sizeof(devAttr));
        if (ncclSuccess != wrap_ibv_query_device(context, &devAttr)) {
          WARN("NET/FuseLink : Unable to query device %s", devices[d]->name);
          if (ncclSuccess != wrap_ibv_close_device(context)) { return ncclInternalError; }
          continue;
        }
        for (int port = 1; port <= devAttr.phys_port_cnt; port++) {
          struct ibv_port_attr portAttr;
          if (ncclSuccess != wrap_ibv_query_port(context, port, &portAttr)) {
            WARN("NET/FuseLink : Unable to query port %d", port);
            continue;
          }
          if (portAttr.state != IBV_PORT_ACTIVE) continue;
          if (portAttr.link_layer != IBV_LINK_LAYER_INFINIBAND
              && portAttr.link_layer != IBV_LINK_LAYER_ETHERNET) continue;

          // check against user specified HCAs/ports
          if (! (matchIfList(devices[d]->name, port, userIfs, nUserIfs, searchExact) ^ searchNot)) {
            continue;
          }
          TRACE(NCCL_INIT|NCCL_NET,"NET/FuseLink: [%d] %s:%d/%s ", d, devices[d]->name, port,
              portAttr.link_layer == IBV_LINK_LAYER_INFINIBAND ? "IB" : "RoCE");
          pthread_mutex_init(&ncclIbDevs[ncclNIbDevs].lock, NULL);
          ncclIbDevs[ncclNIbDevs].device = d;
          ncclIbDevs[ncclNIbDevs].guid = devAttr.sys_image_guid;
          ncclIbDevs[ncclNIbDevs].port = port;
          ncclIbDevs[ncclNIbDevs].link = portAttr.link_layer;
          ncclIbDevs[ncclNIbDevs].speed = ncclIbSpeed(portAttr.active_speed) * ncclIbWidth(portAttr.active_width);
          ncclIbDevs[ncclNIbDevs].context = context;
          ncclIbDevs[ncclNIbDevs].pdRefs = 0;
          ncclIbDevs[ncclNIbDevs].pd = NULL;
          strncpy(ncclIbDevs[ncclNIbDevs].devName, devices[d]->name, MAXNAMESIZE);
          NCCLCHECK(ncclIbGetPciPath(ncclIbDevs[ncclNIbDevs].devName, &ncclIbDevs[ncclNIbDevs].pciPath, &ncclIbDevs[ncclNIbDevs].realPort));
          ncclIbDevs[ncclNIbDevs].maxQp = devAttr.max_qp;
          ncclIbDevs[ncclNIbDevs].mrCache.capacity = 0;
          ncclIbDevs[ncclNIbDevs].mrCache.population = 0;
          ncclIbDevs[ncclNIbDevs].mrCache.slots = NULL;

          // Enable ADAPTIVE_ROUTING by default on IB networks
          // But allow it to be overloaded by an env parameter
          ncclIbDevs[ncclNIbDevs].ar = (portAttr.link_layer == IBV_LINK_LAYER_INFINIBAND) ? 1 : 0;
          if (ncclParamIbAdaptiveRouting() != -2) ncclIbDevs[ncclNIbDevs].ar = ncclParamIbAdaptiveRouting();

          pthread_create(&ncclIbAsyncThread, NULL, ncclIbAsyncThreadMain, context);
          ncclSetThreadName(ncclIbAsyncThread, "NCCL IbAsync %2d", ncclNIbDevs);
          pthread_detach(ncclIbAsyncThread); // will not be pthread_join()'d
          ncclNIbDevs++;
          nPorts++;
        }
        if (nPorts == 0 && ncclSuccess != wrap_ibv_close_device(context)) { return ncclInternalError; }
      }
      if (nIbDevs && (ncclSuccess != wrap_ibv_free_device_list(devices))) { return ncclInternalError; };
    }
    if (ncclNIbDevs == 0) {
      INFO(NCCL_INIT|NCCL_NET, "NET/FuseLink : No device found.");
    } else {
      char line[1024];
      line[0] = '\0';
      // Determine whether RELAXED_ORDERING is enabled and possible
      for (int d=0; d<ncclNIbDevs; d++) {
        snprintf(line+strlen(line), 1023-strlen(line), " [%d]%s:%d/%s", d, ncclIbDevs[d].devName,
            ncclIbDevs[d].port, ncclIbDevs[d].link == IBV_LINK_LAYER_INFINIBAND ? "IB" : "RoCE");
      }
      line[1023] = '\0';
      char addrline[SOCKET_NAME_MAXLEN+1];
      INFO(NCCL_INIT|NCCL_NET, "NET/FuseLink : Using%s %s; OOB %s:%s", line, ncclIbRelaxedOrderingEnabled ? "[RO]" : "",
           ncclIbIfName, ncclSocketToString(&ncclIbIfAddr, addrline));
    }
    pthread_mutex_unlock(&ncclIbLock);
  }
  int ndevs = ncclNIbDevs;
  int priority_dev = ncclParamFuseLinkPriorityDev();
  fuselink_conn_manager = new FuseLinkConnManager(ndevs, priority_dev);
  fuselink_conn_manager->tx_setup_state_ = FuseLinkConnSetupStateInit;
  fuselink_conn_manager->rx_setup_state_ = FuseLinkConnSetupStateInit;

  // allocate pd for each ib device
  for (int idev = 0; idev < ncclNIbDevs; idev++) {
    if (0 == ncclIbDevs[idev].pdRefs++) {
      ncclResult_t res;
      NCCLCHECKGOTO(wrap_ibv_alloc_pd(&ncclIbDevs[idev].pd, ncclIbDevs[idev].context), res, failure);
      if (0) {
      failure:
        pthread_mutex_unlock(&ncclIbDevs[idev].lock);
        return res;
      }
    }
  }

  char *warn_str = \
   "\n\n=======================\nNET/FuseLink: init done\n\nFuseLink is in very early stage of development, use in caution.\n=======================";

  WARN("%s", warn_str);

  CUDACHECK(cudaGetDeviceCount(&NGPUs));

  char* nic2gpu_str = getenv("FUSELINK_NIC2GPU");
  if (nic2gpu_str != NULL) {
    int nic_idx = 0;
    // nic2gpu_str is a comma separated list of GPU indices
    char* token = strtok(nic2gpu_str, ",");
    while (token != NULL) {
      int gpu_idx = atoi(token);
      if (gpu_idx >= 0 && gpu_idx < MAX_GPU_NUM) {
        NIC2GPU[nic_idx] = gpu_idx;
        nic_idx++;
      } else {
        WARN("FUSELINK_NIC2GPU: invalid GPU index %d", gpu_idx);
      }
      token = strtok(NULL, ",");
    }
    for (int i = nic_idx; i < MAX_NIC_NUM; i++) {
      NIC2GPU[i] = -1;
    }
  }
  char* channel2nic_str = getenv("FUSELINK_CHANNEL2NIC");
  if (channel2nic_str != NULL) {
    int channel_idx = 0;
    char* token = strtok(channel2nic_str, ",");
    while (token != NULL) {
      int nic_idx = atoi(token);
      Channel2NIC[channel_idx] = nic_idx;
      channel_idx++;
      token = strtok(NULL, ",");
    }
    for (int i = channel_idx; i < FUSELINK_MAX_NCHANNEL; i++) {
      Channel2NIC[i] = -1;
    }
  }
  for (int i = 0; i < FUSELINK_MAX_NCHANNEL; i++) {
    INFO(NCCL_INIT|NCCL_NET, "NET/FuseLink : channel %d nic %d", i, Channel2NIC[i]);
  }

  return ncclSuccess;
}

ncclResult_t ncclIbDevices(int* ndev) {
  *ndev = ncclNIbDevs;
  return ncclSuccess;
}

// Detect whether GDR can work on a given NIC with the current CUDA device
// Returns :
// ncclSuccess : GDR works
// ncclSystemError : no module or module loaded but not supported by GPU
ncclResult_t ncclIbGdrSupport(int ibDev) {
  static int moduleLoaded = -1;
  if (moduleLoaded == -1) {
    // Check for the nv_peer_mem module being loaded
    moduleLoaded = ((access("/sys/kernel/mm/memory_peers/nv_mem/version", F_OK) == -1) &&
                    // Also support the new nvidia-peermem module
                    (access("/sys/kernel/mm/memory_peers/nvidia-peermem/version", F_OK) == -1)) ? 0 : 1;
  }
  if (moduleLoaded == 0) return ncclSystemError;
  return ncclSuccess;
}

// Detect whether DMA-BUF support is present in the kernel
// Returns :
// ncclSuccess : DMA-BUF support is available
// ncclSystemError : DMA-BUF is not supported by the kernel
ncclResult_t ncclIbDmaBufSupport(int dev) {
  static int dmaBufSupported = -1;
  if (dmaBufSupported == -1) {
    ncclResult_t res;
    struct ibv_pd* pd;
    struct ibv_context* ctx;
    ctx = ncclIbDevs[dev].context;
    NCCLCHECKGOTO(wrap_ibv_alloc_pd(&pd, ctx), res, failure);
    // Test kernel DMA-BUF support with a dummy call (fd=-1)
    (void) wrap_direct_ibv_reg_dmabuf_mr(pd, 0ULL/*offset*/, 0ULL/*len*/, 0ULL/*iova*/, -1/*fd*/, 0/*flags*/);
    // ibv_reg_dmabuf_mr() will fail with EOPNOTSUPP/EPROTONOSUPPORT if not supported (EBADF otherwise)
    dmaBufSupported = (errno != EOPNOTSUPP && errno != EPROTONOSUPPORT) ? 1 : 0;
    NCCLCHECKGOTO(wrap_ibv_dealloc_pd(pd), res, failure);
  }
  if (dmaBufSupported == 0) return ncclSystemError;
  return ncclSuccess;
failure:
  dmaBufSupported = 0;
  return ncclSystemError;
}

#define NCCL_NET_IB_MAX_RECVS 8

ncclResult_t ncclIbGetProperties(int dev, ncclNetProperties_t* props) {
  props->name = ncclIbDevs[dev].devName;
  props->pciPath = ncclIbDevs[dev].pciPath;
  props->guid = ncclIbDevs[dev].guid;
  props->ptrSupport = NCCL_PTR_HOST;
  if (ncclIbGdrSupport(dev) == ncclSuccess) {
    props->ptrSupport |= NCCL_PTR_CUDA; // GDR support via nv_peermem
  }
  if (ncclIbDmaBufSupport(dev) == ncclSuccess) {
    props->ptrSupport |= NCCL_PTR_DMABUF; // GDR support via DMA-BUF
  }
  props->speed = ncclIbDevs[dev].speed;
  props->latency = 0; // Not set
  props->port = ncclIbDevs[dev].port + ncclIbDevs[dev].realPort;
  props->maxComms = ncclIbDevs[dev].maxQp;
  props->maxRecvs = NCCL_NET_IB_MAX_RECVS;
  props->netDeviceType    = NCCL_NET_DEVICE_HOST;
  props->netDeviceVersion = NCCL_NET_DEVICE_INVALID_VERSION;
  return ncclSuccess;
}

// We need to support NCCL_NET_MAX_REQUESTS for each concurrent receive
#define MAX_REQUESTS (NCCL_NET_MAX_REQUESTS*NCCL_NET_IB_MAX_RECVS)
static_assert(MAX_REQUESTS <= 256, "request id are encoded in wr_id and we need up to 8 requests ids per completion");

#define NCCL_IB_MAX_QPS 128

struct ncclIbQpInfo {
  uint32_t lid;
  uint8_t ib_port;
  uint8_t link_layer;
  uint32_t qpn[NCCL_IB_MAX_QPS];

  // Fields needed for ece (enhanced connection establishment)
  struct ibv_ece ece[NCCL_IB_MAX_QPS];
  int ece_supported[NCCL_IB_MAX_QPS];

  // For RoCE
  uint64_t spn;
  uint64_t iid;
  enum ibv_mtu mtu;

  // FIFO RDMA info
  uint32_t fifoRkey;
  uint64_t fifoAddr;
};

enum ncclIbCommState {
  ncclIbCommStateStart = 0,
  ncclIbCommStateConnect = 1,
  ncclIbCommStateAccept = 3,
  ncclIbCommStateSend = 4,
  ncclIbCommStateRecv = 5,
  ncclIbCommStateConnecting = 6,
  ncclIbCommStateConnected = 7,
  ncclIbCommStatePendingReady = 8,
};

struct ncclIbCommStage {
  enum ncclIbCommState state;
  int offset;
  void* buffer;
  void* comm;
};

struct ncclIbHandle {
  union ncclSocketAddress connectAddr; // Filled by the target
  uint64_t magic; // random number to help debugging
  struct ncclIbCommStage stage; // Used by the other side when connecting
  int channelId;
};

// Retain local and remote RoCE addresses for error logging
struct ncclIbGidInfo {
  uint8_t link_layer;
  union ibv_gid localGid;
  union ibv_gid remoteGid;
};

#define NCCL_NET_IB_REQ_UNUSED 0
#define NCCL_NET_IB_REQ_SEND 1
#define NCCL_NET_IB_REQ_RECV 2
#define NCCL_NET_IB_REQ_FLUSH 3
const char* reqTypeStr[] = { "Unused", "Send", "Recv", "Flush" };

struct ncclIbRequest {
  FuseLinkComm* control_comm; // control messages, like post fifo
  FuseLinkComm* data_comm;    // data messages, carried by isend/irecv
  int type;
  int events;
  struct ncclIbGidInfo* gidInfo;
  int nreqs;
  ncclSocket* sock;
  ncclIbRequest* reqs;
  union {
    struct {
      int size;
      void* data;
      uint32_t lkey;
      int offset;
    } send;
    struct {
      int sizes[NCCL_NET_IB_MAX_RECVS];
    } recv;
  };
  uint64_t* posted;
  uint64_t* received;
  uint64_t* flushed;
  uint64_t* transmitted;
  uint64_t* done;
  int* nsteps;
};

struct ncclIbVerbs {
  int dev;
  struct ibv_pd* pd; // duplicate of ncclIbDevs[dev].pd
  struct ibv_cq* cq;
  uint64_t pad[1];
};

struct ncclIbListenComm {
  int dev;
  struct ncclSocket sock;
  struct ncclIbCommStage stage;
  int channelId;
};

struct ncclIbSendFifo {
  uint64_t addr;
  int      size;
  uint32_t rkey;
  uint32_t nreqs;
  uint32_t tag;
  uint64_t idx;
  int fuselink_offset;
  char padding[28]; // 32 bytes alignment
};

struct ncclIbSendComm {
  /*May be we don't need data, control seperation, but keep it here for coherence with fl recv comm*/
  FuseLinkComm* control_comm;
  FuseLinkComm* data_comm;
  struct ibv_mr* fifoMr;
  struct ncclIbRequest reqs[MAX_REQUESTS];
  int ready;
  int ar;
  struct ncclIbSendFifo fifo[MAX_REQUESTS][NCCL_NET_IB_MAX_RECVS];
  uint64_t fifoHead;
  struct ncclIbRequest* fifoReqs[MAX_REQUESTS][NCCL_NET_IB_MAX_RECVS];
  struct ibv_send_wr wrs[NCCL_NET_IB_MAX_RECVS+1];
  struct ibv_sge sges[NCCL_NET_IB_MAX_RECVS];
  struct ncclSocket sock;


  /*move to fuselinkcomm*/
  // struct ibv_qp* qps[NCCL_IB_MAX_QPS];
  /*stay here, associated with the control comm*/
  struct ncclIbGidInfo gidInfo;
  int64_t n_finished; // number of finished requests

  // int initialized;
  /**
   * FuseLinkComm related fields
   * use channel id to locate the available send data links
   */
  int channelId;
  uint64_t* posted;
  uint64_t* received;
  uint64_t* flushed;
  uint64_t* transmitted;
  uint64_t* done;
  int* nsteps;

  uintptr_t request_addr;
};
// The SendFifo needs to be 32-byte aligned and each element needs
// to be a 32-byte multiple, so that an entry does not get split and
// written out of order when IB Relaxed Ordering is enabled
static_assert((offsetof(struct ncclIbSendComm, fifo) % 32) == 0, "ncclIbSendComm fifo must be 32-byte aligned");
static_assert((sizeof(struct ncclIbSendFifo) % 32) == 0, "ncclIbSendFifo element size must be 32-byte multiples");

struct ncclIbGpuFlush {
  int enabled;
  int hostMem;
  struct ibv_mr* hostMr;
  struct ibv_sge sge;
  struct ibv_qp* qp;
};

struct ncclIbRemFifo {
  struct ncclIbSendFifo elems[MAX_REQUESTS][NCCL_NET_IB_MAX_RECVS];
  uint64_t fifoTail;
  uint64_t addr;
  uint32_t rkey;
  uint32_t flags;
  struct ibv_mr* mr;
  struct ibv_sge sge;
};

struct ncclIbRecvComm {
  FuseLinkComm* control_comm;
  FuseLinkComm* data_comm;
  int channelId;
  int nqps;
  int ready;
  int qpIndex;
  struct ncclIbRemFifo remFifo;
  struct ncclSocket sock;
  struct ibv_qp* qps[NCCL_IB_MAX_QPS];
  struct ncclIbGpuFlush gpuFlush;
  struct ncclIbGidInfo gidInfo;

  ncclIbRequest reqs[MAX_REQUESTS];

  uint32_t txAvailable; // a mask indicating tx nic availability.
  int64_t n_finished;
  int fuselink_offset;

  // for hacking upper layer info
  // int initialized;
  uint64_t* posted;
  uint64_t* received;
  uint64_t* flushed;
  uint64_t* transmitted;
  uint64_t* done;
  int* nsteps;
  // done
  uintptr_t request_addr;
};
static_assert((offsetof(struct ncclIbRecvComm, remFifo) % 32) == 0, "ncclIbSendComm fifo must be 32-byte aligned");

NCCL_PARAM(IbQpsPerConn, "IB_QPS_PER_CONNECTION", 1);

static uintptr_t get_addr_abs(uintptr_t l, uintptr_t r) {
  return l > r ? l - r : r - l;
}

ncclResult_t FuseLinkInitSendComm(int dev, struct ibv_context* ctx, struct FuseLinkComm** fl_snd_comm) {
  *fl_snd_comm = new FuseLinkComm(dev);
  INFO(NCCL_INIT|NCCL_NET, "init send comm, dev %d", dev);
  pthread_mutex_lock(&ncclIbDevs[dev].lock);
  if (0 == ncclIbDevs[dev].pdRefs++) {
    ncclResult_t res = wrap_ibv_alloc_pd(&ncclIbDevs[dev].pd, ctx);
    if (res != ncclSuccess) {
      pthread_mutex_unlock(&ncclIbDevs[dev].lock);
      return res;
    }
  }
  pthread_mutex_unlock(&ncclIbDevs[dev].lock);

  (*fl_snd_comm)->pd = ncclIbDevs[dev].pd;
  NCCLCHECK(wrap_ibv_create_cq(&(*fl_snd_comm)->cq, ctx, 2*MAX_REQUESTS*ncclParamIbQpsPerConn(), NULL, NULL, 0));

  return ncclSuccess;
}

ncclResult_t ncclIbDestroyVerbs(struct ncclIbVerbs* verbs) {
  ncclResult_t res;
  NCCLCHECK(wrap_ibv_destroy_cq(verbs->cq));

  pthread_mutex_lock(&ncclIbDevs[verbs->dev].lock);
  if (0 == --ncclIbDevs[verbs->dev].pdRefs) {
    NCCLCHECKGOTO(wrap_ibv_dealloc_pd(ncclIbDevs[verbs->dev].pd), res, returning);
  }
  res = ncclSuccess;
returning:
  pthread_mutex_unlock(&ncclIbDevs[verbs->dev].lock);
  return res;
}

ncclResult_t FuseLinkCreateQp(uint8_t ib_port, struct ibv_cq* cq, struct ibv_pd* pd, int access_flags, struct ibv_qp** qp) {
  struct ibv_qp_init_attr qpInitAttr;
  memset(&qpInitAttr, 0, sizeof(struct ibv_qp_init_attr));
  qpInitAttr.send_cq = cq;
  qpInitAttr.recv_cq = cq;
  qpInitAttr.qp_type = IBV_QPT_RC;
  // We might send 2 messages per send (RDMA and RDMA_WITH_IMM)
  qpInitAttr.cap.max_send_wr = 2*MAX_REQUESTS;
  qpInitAttr.cap.max_recv_wr = MAX_REQUESTS;
  qpInitAttr.cap.max_send_sge = 1;
  qpInitAttr.cap.max_recv_sge = 1;
  qpInitAttr.cap.max_inline_data = ncclParamIbUseInline() ? sizeof(struct ncclIbSendFifo) : 0;
  NCCLCHECK(wrap_ibv_create_qp(qp, pd, &qpInitAttr));
  struct ibv_qp_attr qpAttr;
  memset(&qpAttr, 0, sizeof(struct ibv_qp_attr));
  qpAttr.qp_state = IBV_QPS_INIT;
  qpAttr.pkey_index = ncclParamIbPkey();
  qpAttr.port_num = ib_port;
  qpAttr.qp_access_flags = access_flags;
  NCCLCHECK(wrap_ibv_modify_qp(*qp, &qpAttr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS));
  return ncclSuccess;
}

ncclResult_t ncclIbCreateQp(uint8_t ib_port, struct ncclIbVerbs* verbs, int access_flags, struct ibv_qp** qp) {
  struct ibv_qp_init_attr qpInitAttr;
  memset(&qpInitAttr, 0, sizeof(struct ibv_qp_init_attr));
  qpInitAttr.send_cq = verbs->cq;
  qpInitAttr.recv_cq = verbs->cq;
  qpInitAttr.qp_type = IBV_QPT_RC;
  // We might send 2 messages per send (RDMA and RDMA_WITH_IMM)
  qpInitAttr.cap.max_send_wr = 2*MAX_REQUESTS;
  qpInitAttr.cap.max_recv_wr = MAX_REQUESTS;
  qpInitAttr.cap.max_send_sge = 1;
  qpInitAttr.cap.max_recv_sge = 1;
  qpInitAttr.cap.max_inline_data = ncclParamIbUseInline() ? sizeof(struct ncclIbSendFifo) : 0;
  NCCLCHECK(wrap_ibv_create_qp(qp, verbs->pd, &qpInitAttr));
  struct ibv_qp_attr qpAttr;
  memset(&qpAttr, 0, sizeof(struct ibv_qp_attr));
  qpAttr.qp_state = IBV_QPS_INIT;
  qpAttr.pkey_index = ncclParamIbPkey();
  qpAttr.port_num = ib_port;
  qpAttr.qp_access_flags = access_flags;
  NCCLCHECK(wrap_ibv_modify_qp(*qp, &qpAttr, IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS));
  return ncclSuccess;
}

ncclResult_t ncclIbRtrQp(struct ibv_qp* qp, uint32_t qpn, struct ncclIbQpInfo* info) {
  struct ibv_qp_attr qpAttr;
  memset(&qpAttr, 0, sizeof(struct ibv_qp_attr));
  qpAttr.qp_state = IBV_QPS_RTR;
  qpAttr.path_mtu = info->mtu;
  qpAttr.dest_qp_num = qpn;
  qpAttr.rq_psn = 0;
  qpAttr.max_dest_rd_atomic = 1;
  qpAttr.min_rnr_timer = 12;
  if (info->link_layer == IBV_LINK_LAYER_ETHERNET) {
    qpAttr.ah_attr.is_global = 1;
    qpAttr.ah_attr.grh.dgid.global.subnet_prefix = info->spn;
    qpAttr.ah_attr.grh.dgid.global.interface_id = info->iid;
    qpAttr.ah_attr.grh.flow_label = 0;
    qpAttr.ah_attr.grh.sgid_index = ncclParamIbGidIndex();
    qpAttr.ah_attr.grh.hop_limit = 255;
    qpAttr.ah_attr.grh.traffic_class = ncclParamIbTc();
  } else {
    qpAttr.ah_attr.is_global = 0;
    qpAttr.ah_attr.dlid = info->lid;
  }
  qpAttr.ah_attr.sl = ncclParamIbSl();
  qpAttr.ah_attr.src_path_bits = 0;
  qpAttr.ah_attr.port_num = info->ib_port;
  NCCLCHECK(wrap_ibv_modify_qp(qp, &qpAttr, IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER));
  return ncclSuccess;
}

ncclResult_t ncclIbRtsQp(struct ibv_qp* qp) {
  struct ibv_qp_attr qpAttr;
  memset(&qpAttr, 0, sizeof(struct ibv_qp_attr));
  qpAttr.qp_state = IBV_QPS_RTS;
  qpAttr.timeout = ncclParamIbTimeout();
  qpAttr.retry_cnt = ncclParamIbRetryCnt();
  qpAttr.rnr_retry = 7;
  qpAttr.sq_psn = 0;
  qpAttr.max_rd_atomic = 1;
  NCCLCHECK(wrap_ibv_modify_qp(qp, &qpAttr, IBV_QP_STATE | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC));
  return ncclSuccess;
}

pthread_mutex_t channelCounterMutex = PTHREAD_MUTEX_INITIALIZER;
int channelCounter = 0;

static int ncclIbAllocateChannel() {
  pthread_mutex_lock(&channelCounterMutex);
  int channel = channelCounter++;
  pthread_mutex_unlock(&channelCounterMutex);
  return channel;
}

NCCL_PARAM(FuseLinkNChannels, "FUSELINK_NCHANNELS", 4);

static int nChannels = ncclParamFuseLinkNChannels();

ncclResult_t ncclIbListen(int dev, void* opaqueHandle, void** listenComm) {
  struct ncclIbListenComm* comm;
  NCCLCHECK(ncclCalloc(&comm, 1));
  struct ncclIbHandle* handle = (struct ncclIbHandle*) opaqueHandle;
  static_assert(sizeof(struct ncclIbHandle) < NCCL_NET_HANDLE_MAXSIZE, "ncclIbHandle size too large");
  memset(handle, 0, sizeof(struct ncclIbHandle));
  handle->magic = NCCL_SOCKET_MAGIC;
  handle->channelId = ncclIbAllocateChannel();
  comm->channelId = handle->channelId;
  
  // use the specified dev if provided, otherwise use the dev given by NCCL
  comm->dev = Channel2NIC[handle->channelId % nChannels] == -1 ? dev : Channel2NIC[handle->channelId % nChannels];
  INFO(NCCL_INIT|NCCL_NET, "channel id %d listen comm %p dev %d", comm->channelId, comm, comm->dev);
  NCCLCHECK(ncclSocketInit(&comm->sock, &ncclIbIfAddr, handle->magic, ncclSocketTypeNetIb, NULL, 1));
  NCCLCHECK(ncclSocketListen(&comm->sock));
  NCCLCHECK(ncclSocketGetAddr(&comm->sock, &handle->connectAddr));
  *listenComm = comm;
  return ncclSuccess;
}

ncclResult_t ncclIbConnect(int dev, void* opaqueHandle, void** sendComm, ncclNetDeviceHandle_t** /*sendDevComm*/) {
  struct ncclIbHandle* handle = (struct ncclIbHandle*) opaqueHandle;
  struct ncclIbCommStage* stage = &handle->stage;
  int channelId = handle->channelId;
  dev = Channel2NIC[channelId % nChannels] == -1 ? dev : Channel2NIC[channelId % nChannels]; // update connect NIC
  // INFO(NCCL_INIT|NCCL_NET, "NET/FuseLink : thread %p Using channel %d for connection", pthread_self(), handle->channelId);

  struct ncclIbSendComm* comm = (struct ncclIbSendComm*)stage->comm;
  int ready;
  *sendComm = NULL;

  if (stage->state == ncclIbCommStateConnect)    goto ib_connect_check;
  if (stage->state == ncclIbCommStateSend)       goto ib_send;
  if (stage->state == ncclIbCommStateConnecting) goto ib_connect;
  if (stage->state == ncclIbCommStateConnected)  goto ib_send_ready;
  if (stage->state != ncclIbCommStateStart) {
    WARN("Error: trying to connect already connected sendComm");
    return ncclInternalError;
  }

  NCCLCHECK(ncclIbMalloc((void**)&comm, sizeof(struct ncclIbSendComm)));
  comm->channelId = channelId;
  comm->n_finished = 0;
  NCCLCHECK(ncclSocketInit(&comm->sock, &handle->connectAddr, handle->magic, ncclSocketTypeNetIb, NULL, 1));
  stage->comm = comm;
  stage->state = ncclIbCommStateConnect;
  NCCLCHECK(ncclSocketConnect(&comm->sock));

ib_connect_check:
  /* since ncclSocketConnect is async, we must check if connection is complete */
  NCCLCHECK(ncclSocketReady(&comm->sock, &ready));
  if (!ready) return ncclSuccess;


  // RDMA connection set up, including control qp and data qp
  struct ibv_context* ctx;
  struct ncclIbQpInfo qpInfos[MAX_NIC_NUM];
  for (uint nicdev = 0; nicdev < ncclNIbDevs; nicdev++) {
    INFO(NCCL_INIT|NCCL_NET, "NET/FuseLink : set up send channel %d nicdev %d", channelId, nicdev);
    ctx = ncclIbDevs[nicdev].context;
    // similar to ncclIbInitIbVerbs
    NCCLCHECK(FuseLinkInitSendComm(nicdev, ctx, &fuselink_send_comm_[channelId][nicdev]));
    if (fuselink_send_comm_[channelId][nicdev] == NULL) {
      return ncclInternalError;
    }
    FuseLinkComm* fl_send_comm = fuselink_send_comm_[channelId][nicdev];
    uint8_t ib_port;
    ib_port = ncclIbDevs[nicdev].port;
    NCCLCHECK(FuseLinkCreateQp(ib_port, fl_send_comm->cq, fl_send_comm->pd, \
       IBV_ACCESS_REMOTE_WRITE, &fl_send_comm->qp));

    struct ibv_port_attr portAttr;
    NCCLCHECK(wrap_ibv_query_port(ctx, ib_port, &portAttr));

    qpInfos[nicdev].ib_port = ib_port;

    // use 1 qp
    qpInfos[nicdev].qpn[0] = fl_send_comm->qp->qp_num; // fuselink qpn
    // query ece capabilities
    
    NCCLCHECK(wrap_ibv_query_ece(fl_send_comm->qp, &qpInfos[nicdev].ece[0], &qpInfos[nicdev].ece_supported[0]));

    qpInfos[nicdev].mtu = portAttr.active_mtu;
    qpInfos[nicdev].lid = portAttr.lid;
    qpInfos[nicdev].link_layer = fl_send_comm->gidInfo.link_layer = portAttr.link_layer;

    if (qpInfos[nicdev].link_layer == IBV_LINK_LAYER_ETHERNET) {
      // query gid
      NCCLCHECK(wrap_ibv_query_gid(ncclIbDevs[nicdev].context, ncclIbDevs[nicdev].port, \
         ncclParamIbGidIndex(), &fl_send_comm->gidInfo.localGid));
      qpInfos[nicdev].spn = fl_send_comm->gidInfo.localGid.global.subnet_prefix;
      qpInfos[nicdev].iid = fl_send_comm->gidInfo.localGid.global.interface_id;

      // update spn and iid
    }

    if (nicdev == dev) {
      // use as the control qp
      NCCLCHECK(wrap_ibv_reg_mr(&comm->fifoMr, ncclIbDevs[nicdev].pd, comm->fifo, \
         sizeof(struct ncclIbSendFifo)*MAX_REQUESTS*NCCL_NET_IB_MAX_RECVS, \
          IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_REMOTE_READ));
      qpInfos[nicdev].fifoRkey = comm->fifoMr->rkey;
      qpInfos[nicdev].fifoAddr = (uint64_t)comm->fifo;
    }
  }
  

  stage->state = ncclIbCommStateSend;
  stage->offset = 0;
  NCCLCHECK(ncclIbMalloc((void**)&stage->buffer, sizeof(struct ncclIbQpInfo) * MAX_NIC_NUM));
  memcpy(stage->buffer, qpInfos, sizeof(struct ncclIbQpInfo) * MAX_NIC_NUM);

ib_send:
  NCCLCHECK(ncclSocketProgress(NCCL_SOCKET_SEND, &comm->sock, stage->buffer, sizeof(ncclIbQpInfo) * MAX_NIC_NUM, &stage->offset));
  if (stage->offset != sizeof(ncclIbQpInfo) * MAX_NIC_NUM) return ncclSuccess;

  stage->state = ncclIbCommStateConnecting;
  stage->offset = 0;
  // Clear the staging buffer for re-use
  memset(stage->buffer, 0, sizeof(struct ncclIbQpInfo) * MAX_NIC_NUM);

ib_connect:
  struct ncclIbQpInfo remQpInfos[MAX_NIC_NUM];
  NCCLCHECK(ncclSocketProgress(NCCL_SOCKET_RECV, &comm->sock, stage->buffer, sizeof(struct ncclIbQpInfo) * MAX_NIC_NUM, &stage->offset));
  if (stage->offset != sizeof(struct ncclIbQpInfo) * MAX_NIC_NUM) return ncclSuccess;

  memcpy(remQpInfos, stage->buffer, sizeof(struct ncclIbQpInfo) * MAX_NIC_NUM);

  for (uint nicdev = 0; nicdev < ncclNIbDevs; nicdev++) {
    struct ncclIbQpInfo& remQpInfo = remQpInfos[nicdev];
    struct ibv_qp* qp = fuselink_send_comm_[channelId][nicdev]->qp;
    struct ncclIbQpInfo& qpInfo = qpInfos[nicdev];
    if (remQpInfo.ece_supported[0] && qpInfo.ece_supported[0])
      NCCLCHECK(wrap_ibv_set_ece(qp, &remQpInfo.ece[0], &qpInfo.ece_supported[0]));

    NCCLCHECK(ncclIbRtrQp(qp, remQpInfo.qpn[0], &remQpInfo));
    NCCLCHECK(ncclIbRtsQp(qp));
  }

  comm->ready = 1;
  comm->request_addr = 0;
  stage->state = ncclIbCommStateConnected;
  stage->offset = 0;

ib_send_ready:
  NCCLCHECK(ncclSocketProgress(NCCL_SOCKET_SEND, &comm->sock, &comm->ready, sizeof(int), &stage->offset));
  if (stage->offset != sizeof(int)) return ncclSuccess;

  free(stage->buffer);
  stage->state = ncclIbCommStateStart;

  *sendComm = comm;
  comm->control_comm = fuselink_send_comm_[channelId][dev];
  INFO(NCCL_INIT, "send Control comm dev %d", comm->control_comm->dev_);
  comm->data_comm = fuselink_send_comm_[channelId][dev];
  INFO(NCCL_INIT|NCCL_NET, "channel id %d sendcomm %p control at %p dev %d", comm->channelId, comm, comm->control_comm, comm->control_comm->dev_);
  return ncclSuccess;
}

NCCL_PARAM(IbGdrFlushDisable, "GDR_FLUSH_DISABLE", 1);


ncclResult_t FuseLinkInitRecvComm(int dev, struct ibv_context* ctx, FuseLinkComm** recvComm) {
  *recvComm = new FuseLinkComm(dev);
  INFO(NCCL_INIT|NCCL_NET, "init recv comm, dev %d", dev);
  pthread_mutex_lock(&ncclIbDevs[dev].lock);
  if (0 == ncclIbDevs[dev].pdRefs++) {
    ncclResult_t res = wrap_ibv_alloc_pd(&ncclIbDevs[dev].pd, ctx);
    if (res != ncclSuccess) {
      pthread_mutex_unlock(&ncclIbDevs[dev].lock);
      return res;
    }
  }
  pthread_mutex_unlock(&ncclIbDevs[dev].lock);

  (*recvComm)->pd = ncclIbDevs[dev].pd;
  NCCLCHECK(wrap_ibv_create_cq(&(*recvComm)->cq, ctx, 2*MAX_REQUESTS*ncclParamIbQpsPerConn(), NULL, NULL, 0));

  return ncclSuccess;
}

ncclResult_t ncclIbAccept(void* listenComm, void** recvComm, ncclNetDeviceHandle_t** /*recvDevComm*/) {
  struct ncclIbListenComm* lComm = (struct ncclIbListenComm*)listenComm;
  struct ncclIbCommStage* stage = &lComm->stage;
  struct ncclIbRecvComm* rComm = (struct ncclIbRecvComm*)stage->comm;
  int ready;
  *recvComm = NULL;
  int channelId = lComm->channelId;


  if (stage->state == ncclIbCommStateAccept) goto ib_accept_check;
  if (stage->state == ncclIbCommStateRecv) goto ib_recv;
  if (stage->state == ncclIbCommStateSend) goto ib_send;
  if (stage->state == ncclIbCommStatePendingReady) goto ib_recv_ready;
  if (stage->state != ncclIbCommStateStart) {
    WARN("Listencomm in unknown state %d", stage->state);
    return ncclInternalError;
  }

  NCCLCHECK(ncclIbMalloc((void**)&rComm, sizeof(struct ncclIbRecvComm)));
  stage->comm = rComm;
  stage->state = ncclIbCommStateAccept;
  // INFO(NCCL_INIT|NCCL_NET, "NET/FuseLink : thread %p Using channel %d for ACCEPT", pthread_self(), lComm->channelId);
  NCCLCHECK(ncclSocketInit(&rComm->sock));
  NCCLCHECK(ncclSocketAccept(&rComm->sock, &lComm->sock));

ib_accept_check:
  NCCLCHECK(ncclSocketReady(&rComm->sock, &ready));
  if (!ready) return ncclSuccess;

  struct ncclIbQpInfo remQpInfos[MAX_NIC_NUM];
  struct ncclIbQpInfo qpInfos[MAX_NIC_NUM];
  stage->state = ncclIbCommStateRecv;
  stage->offset = 0;
  NCCLCHECK(ncclIbMalloc((void**)&stage->buffer, sizeof(struct ncclIbQpInfo) * MAX_NIC_NUM));

ib_recv:
  NCCLCHECK(ncclSocketProgress(NCCL_SOCKET_RECV, &rComm->sock, stage->buffer, sizeof(struct ncclIbQpInfo) * MAX_NIC_NUM, &stage->offset));
  if (stage->offset != sizeof(struct ncclIbQpInfo) * MAX_NIC_NUM) return ncclSuccess;

  /* copy back the received info */
  memcpy(remQpInfos, stage->buffer, sizeof(struct ncclIbQpInfo) * MAX_NIC_NUM);

  // rComm->gidInfo.remoteGid.global.subnet_prefix = remQpInfo.spn;
  // rComm->gidInfo.remoteGid.global.interface_id = remQpInfo.iid;

  // IB setup with multiple qps over multiple NICs
  for (uint nicdev = 0; nicdev < ncclNIbDevs; nicdev++) {
    INFO(NCCL_INIT|NCCL_NET, "NET/FuseLink : set up recv channel %d nicdev %d", channelId, nicdev);
    struct ibv_context* ctx = ncclIbDevs[nicdev].context;
    uint8_t ib_port = ncclIbDevs[nicdev].port;
    struct ibv_port_attr portAttr;
    NCCLCHECK(wrap_ibv_query_port(ctx, ib_port, &portAttr));

    FuseLinkInitRecvComm(nicdev, ctx, &fuselink_recv_comm_[channelId][nicdev]);
    FuseLinkComm* fl_recv_comm = fuselink_recv_comm_[channelId][nicdev];

    NCCLCHECK(wrap_ibv_query_gid(ctx, ib_port, ncclParamIbGidIndex(), &fl_recv_comm->gidInfo.localGid));

    NCCLCHECK(FuseLinkCreateQp(ib_port, fl_recv_comm->cq, fl_recv_comm->pd, \
       IBV_ACCESS_REMOTE_WRITE, &fl_recv_comm->qp));

    struct ncclIbQpInfo& remQpInfo = remQpInfos[nicdev];
    struct ncclIbQpInfo& qpInfo = qpInfos[nicdev];
    remQpInfo.mtu = (enum ibv_mtu)std::min(remQpInfo.mtu, portAttr.active_mtu);

    // set up qp
    if (remQpInfo.ece_supported[0]) {
      NCCLCHECK(wrap_ibv_set_ece(fl_recv_comm->qp, &remQpInfo.ece[0], &qpInfo.ece_supported[0]));
      if (qpInfo.ece_supported[0]) {
        NCCLCHECK(wrap_ibv_query_ece(fl_recv_comm->qp, &qpInfo.ece[0], &qpInfo.ece_supported[0]));
      }
    }

    NCCLCHECK(ncclIbRtrQp(fl_recv_comm->qp, remQpInfo.qpn[0], &remQpInfo));
    NCCLCHECK(ncclIbRtsQp(fl_recv_comm->qp));
  }

  rComm->control_comm = fuselink_recv_comm_[channelId][lComm->dev];
  INFO(NCCL_INIT, "recv Control comm dev %d", rComm->control_comm->dev_);
  rComm->data_comm = fuselink_recv_comm_[channelId][lComm->dev];

  // Retain remote fifo info and prepare my RDMA ops
  rComm->remFifo.rkey = remQpInfos[lComm->dev].fifoRkey;
  rComm->remFifo.addr = remQpInfos[lComm->dev].fifoAddr;
  NCCLCHECK(wrap_ibv_reg_mr(&rComm->remFifo.mr, rComm->control_comm->pd, \
    &rComm->remFifo.elems, \
    sizeof(struct ncclIbSendFifo)*MAX_REQUESTS*NCCL_NET_IB_MAX_RECVS,\
    IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_READ));
  rComm->remFifo.sge.lkey = rComm->remFifo.mr->lkey;
  if (ncclParamIbUseInline()) rComm->remFifo.flags = IBV_SEND_INLINE;

  // Allocate Flush dummy buffer for GPU Direct RDMA
  rComm->gpuFlush.enabled = ((ncclIbGdrSupport(lComm->dev) == ncclSuccess || ncclIbDmaBufSupport(lComm->dev) == ncclSuccess)
                             && (ncclParamIbGdrFlushDisable() == 0)) ? 1 : 0;
  if (rComm->gpuFlush.enabled) {
    // ENFOCRE DISABLE GPU FLUSH
    // uint8_t ib_port = ncclIbDevs[rComm->control_comm->dev_].port;
    // NCCLCHECK(wrap_ibv_reg_mr(&rComm->gpuFlush.hostMr, rComm->control_comm->pd, &rComm->gpuFlush.hostMem, sizeof(int), IBV_ACCESS_LOCAL_WRITE));
    // rComm->gpuFlush.sge.addr = (uint64_t)&rComm->gpuFlush.hostMem;
    // rComm->gpuFlush.sge.length = 1;
    // rComm->gpuFlush.sge.lkey = rComm->gpuFlush.hostMr->lkey;
    // NCCLCHECK(ncclIbCreateQp(ib_port, &rComm->verbs, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ, &rComm->gpuFlush.qp));
    // struct ncclIbQpInfo localQpInfo;
    // localQpInfo.lid=portAttr.lid;
    // localQpInfo.link_layer=portAttr.link_layer;
    // localQpInfo.ib_port=ib_port;
    // localQpInfo.spn=rComm->gidInfo.localGid.global.subnet_prefix;
    // localQpInfo.iid=rComm->gidInfo.localGid.global.interface_id;
    // localQpInfo.mtu=portAttr.active_mtu;
    // NCCLCHECK(ncclIbRtrQp(rComm->gpuFlush.qp, rComm->gpuFlush.qp->qp_num, &localQpInfo));
    // NCCLCHECK(ncclIbRtsQp(rComm->gpuFlush.qp));
  }

  // Fill Handles
  for (uint nicdev = 0; nicdev < ncclNIbDevs; nicdev++) {
    struct ncclIbQpInfo& qpInfo = qpInfos[nicdev];
    struct ibv_port_attr portAttr;
    struct ibv_context* ctx;
    ctx = ncclIbDevs[nicdev].context;
    uint8_t ib_port = ncclIbDevs[nicdev].port;
    NCCLCHECK(wrap_ibv_query_port(ctx, ib_port, &portAttr));
    qpInfo.lid=portAttr.lid;
    qpInfo.link_layer= portAttr.link_layer;
    fuselink_recv_comm_[channelId][nicdev]->gidInfo.link_layer = portAttr.link_layer;
    qpInfo.ib_port=ib_port;
    qpInfo.qpn[0] = fuselink_recv_comm_[channelId][nicdev]->qp->qp_num;
    qpInfo.spn = fuselink_recv_comm_[channelId][nicdev]->gidInfo.localGid.global.subnet_prefix;
    qpInfo.iid = fuselink_recv_comm_[channelId][nicdev]->gidInfo.localGid.global.interface_id;
    qpInfo.mtu = remQpInfos[nicdev].mtu;
  }
  // qpInfo.lid=portAttr.lid;
  // qpInfo.link_layer= rComm->gidInfo.link_layer = portAttr.link_layer;
  // qpInfo.ib_port=ib_port;
  // for (int q=0; q<rComm->nqps; q++) qpInfo.qpn[q]=rComm->qps[q]->qp_num;
  // qpInfo.spn=rComm->gidInfo.localGid.global.subnet_prefix;
  // qpInfo.iid=rComm->gidInfo.localGid.global.interface_id;
  // qpInfo.mtu=remQpInfo.mtu;

  stage->state = ncclIbCommStateSend;
  stage->offset = 0;
  if (stage->buffer) free(stage->buffer);
  NCCLCHECK(ncclIbMalloc((void**)&stage->buffer, sizeof(struct ncclIbQpInfo) * MAX_NIC_NUM));
  memcpy(stage->buffer, qpInfos, sizeof(struct ncclIbQpInfo) * MAX_NIC_NUM);

ib_send:
  NCCLCHECK(ncclSocketProgress(NCCL_SOCKET_SEND, &rComm->sock, stage->buffer, sizeof(struct ncclIbQpInfo) * MAX_NIC_NUM, &stage->offset));
  if (stage->offset < sizeof(struct ncclIbQpInfo) * MAX_NIC_NUM) return ncclSuccess;

  stage->offset = 0;
  stage->state = ncclIbCommStatePendingReady;

ib_recv_ready:
  NCCLCHECK(ncclSocketProgress(NCCL_SOCKET_RECV,  &rComm->sock, &rComm->ready, sizeof(int), &stage->offset));
  if (stage->offset != sizeof(int)) return ncclSuccess;

  free(stage->buffer);
  *recvComm = rComm;
  INFO(NCCL_INIT, "recv comm %p", rComm);
  rComm->fuselink_offset = -1;
  rComm->n_finished = 0;
  rComm->channelId = lComm->channelId;
  rComm->txAvailable = 0xffffffff; // all available, place holder.
  rComm->request_addr = 0;

  /* reset lComm stage */
  stage->state = ncclIbCommStateStart;
  stage->offset = 0;
  stage->comm = NULL;
  stage->buffer = NULL;
  return ncclSuccess;
}

ncclResult_t ncclIbGetRequest(ncclIbRequest *reqs, struct ncclIbRequest** req) {
  for (int i=0; i<MAX_REQUESTS; i++) {
    ncclIbRequest* r = reqs+i;
    if (r->type == NCCL_NET_IB_REQ_UNUSED) {
      r->events = 1;
      *req = r;
      r->reqs = reqs;
      return ncclSuccess;
    }
  }
  WARN("NET/FuseLink : unable to allocate requests");
  *req = NULL;
  return ncclInternalError;
}
ncclResult_t ncclIbFreeRequest(struct ncclIbRequest* r) {
  r->type = NCCL_NET_IB_REQ_UNUSED;
  return ncclSuccess;
}

ncclResult_t ncclIbTest(void* request, int* done, int* size);


#define MYCUCHECK(cmd) do { \
  CUresult cuCheckRes = cmd; \
  if (cuCheckRes != CUDA_SUCCESS) { \
    const char *errStr;				      \
    (void) cuGetErrorString(cuCheckRes, &errStr);	      \
    WARN("Cuda error %s:%d '%s' returned %d reason %s", __FILE__, __LINE__, #cmd, cuCheckRes, errStr); \
    return ncclUnhandledCudaError; \
  } \
} while(0)

ncclResult_t FuseLinkRegMr(FuseLinkMemRegion* flmr, int dev, uint64_t offset, int fd) {
  int ndevs = 0;
  int regDevs[MAX_NIC_NUM];
  size_t sz = flmr->sz;
  const size_t pageSize = sysconf(_SC_PAGESIZE);
  size_t pages = (flmr->sz + pageSize-1)/pageSize;
  if (flmr->dev == -1) { // cpu mem
    // register only on control dev
    regDevs[ndevs++] = dev;
  } else { // register on all NICs
    for (int i = 0; i < ncclNIbDevs; i++) {
      regDevs[ndevs++] = i;
    }
  }
  ncclResult_t res = ncclSuccess; // init as success and update as error if registration fails
  for (int i = 0; i < ndevs; i++) {
    int idev = regDevs[i]; // register on idev NIC
    // fetch the registered address
    void* regAddr = nullptr;
    if (flmr->dev == -1) // on CPU
      regAddr = (void *) flmr->start_addr;
    else
      regAddr = (void *) flmr->addr[idev]; // the address that is mapped to the other GPUs with direct connection to NIC idev
    struct ibv_mr** mrs = flmr->mr;
    struct ncclIbMrCache* cache = &ncclIbDevs[idev].mrCache;
    INFO(NCCL_INIT, "lock on dev %d", idev);
    pthread_mutex_lock(&ncclIbDevs[idev].lock);
    for (int slot=0; /*true*/; slot++) {
      if (slot == cache->population) { // didn't find in cache
        if (cache->population == cache->capacity) { // must grow cache
          cache->capacity = cache->capacity < 32 ? 32 : 2*cache->capacity;
          res = ncclRealloc(&cache->slots, cache->population, cache->capacity);
          if (res != ncclSuccess) {
            pthread_mutex_unlock(&ncclIbDevs[idev].lock);
            return res;
          }
        }
        // Deregister / register
        struct ibv_mr* mr;
        unsigned int flags = IBV_ACCESS_LOCAL_WRITE|IBV_ACCESS_REMOTE_WRITE|IBV_ACCESS_REMOTE_READ;
        if (ncclIbRelaxedOrderingEnabled) flags |= IBV_ACCESS_RELAXED_ORDERING;
        if (fd != -1) {
          /* DMA-BUF support */
          NCCLCHECKGOTO(wrap_ibv_reg_dmabuf_mr(&mr, ncclIbDevs[idev].pd, offset, flmr->sz, (uintptr_t) regAddr, fd, flags), res, returning);
        } else {
          if (ncclIbRelaxedOrderingEnabled) {
            // Use IBVERBS_1.8 API - needed for IBV_ACCESS_RELAXED_ORDERING support
            NCCLCHECKGOTO(wrap_ibv_reg_mr_iova2(&mr, ncclIbDevs[idev].pd, (void*)regAddr, flmr->sz, (uintptr_t) regAddr, flags), res, returning);
          }
          else {
            INFO(NCCL_INIT, "reg addr %p dev %d size %zu pd %p flags %x", regAddr, idev, flmr->sz, ncclIbDevs[idev].pd, flags);
            NCCLCHECKGOTO(wrap_ibv_reg_mr(&mr, ncclIbDevs[idev].pd, (void *)regAddr, flmr->sz, flags), res, returning);
          }
        }
        INFO(NCCL_INIT,"dev %d regAddr %llx size %lld lkey %x rkey %x start_addr %p", idev, (unsigned long long)regAddr, (long long)flmr->sz, mr->lkey, mr->rkey, regAddr);
        cache->population += 1;
        cache->slots[slot].addr = (uintptr_t) flmr->start_addr;
        cache->slots[slot].pages = pages;
        cache->slots[slot].refs = 1;
        cache->slots[slot].mr = mr;
        mrs[idev] = mr;
        res = ncclSuccess;
        break; // cache search & build finishes
      }
      else if (cache->slots[slot].addr == (uintptr_t) flmr->start_addr && cache->slots[slot].pages == pages) { // found in cache
        cache->slots[slot].refs += 1;
        mrs[idev] = cache->slots[slot].mr;
        res = ncclSuccess;
        break; // cache search & update finishes
      }
    } // search in all slots
    if (res == ncclSuccess) {
      pthread_mutex_unlock(&ncclIbDevs[idev].lock);
      continue; // register on the next NIC
    }
    else { // register failed
      // dereg previous ones and return error
      for (int t = 0; t < idev; t++) {
        if (mrs[t] != NULL) {
          NCCLCHECK(wrap_ibv_dereg_mr(mrs[t]));
          mrs[t] = NULL;
        }
      }
      pthread_mutex_unlock(&ncclIbDevs[idev].lock);
      return res; // error
    }
  }
  /** UNET
   * post mr info to switch
   * nic0 <-> mr0
   * nic1 <-> mr1
   * ...
   * mr[...] sent to switch(address and rkey)
   */
  returning:
  if (res != ncclSuccess) {
    return res;
  }
  return ncclSuccess;
  
}

/* DMA-BUF support */
ncclResult_t ncclIbRegMrDmaBuf(void* comm, void* data, size_t size, int type, uint64_t offset, int fd, void** mhandle) {
  assert(size > 0);

  static __thread uintptr_t pageSize = 0;
  if (pageSize == 0) pageSize = sysconf(_SC_PAGESIZE);

  static_assert(offsetof(struct ncclIbSendComm, control_comm) == offsetof(struct ncclIbRecvComm, control_comm), \
    "control_comm must have the same offset in send and recv comms");
  FuseLinkComm* control_comm = ((ncclIbSendComm *) comm)->control_comm;

  const int dev = control_comm->dev_;
  INFO(NCCL_NET, "comm %p control_comm %p, registering memory on dev %d", comm, control_comm, dev);

  uintptr_t addr = (uintptr_t)data & -pageSize;
  size_t pages = ((uintptr_t)data + size - addr + pageSize-1)/pageSize;

  void *base_addr = NULL;
  size_t base_size = 0;
  int gpu_id = -1;
  if (type == NCCL_PTR_CUDA) {
    MYCUCHECK(cuPointerGetAttribute(&gpu_id, CU_POINTER_ATTRIBUTE_DEVICE_ORDINAL, (CUdeviceptr)data));
    INFO(NCCL_NET, "NCCL passes gpu pointer on gpu_id %d", gpu_id);
    if (gpu_id >= 0) {
      MYCUCHECK(cuMemGetAddressRange((CUdeviceptr*)&base_addr, &base_size, (CUdeviceptr)data));
    } else {
      WARN("NET/FuseLink: gpu_id %d is invalid", gpu_id);
      return ncclInternalError;
    }
  } else {
    base_addr = (void *) addr;
    base_size = pages * pageSize;
    gpu_id = MAX_GPU_NUM;
  }

  pthread_mutex_lock(&addr2flmr_lock);

  if (addr2flmr.find(base_addr) != addr2flmr.end()) {
    // cached, use the existing one
    auto flmr = addr2flmr.at(base_addr);
    pthread_mutex_unlock(&addr2flmr_lock);
    // update nrefs
    pthread_mutex_lock(&flmr->lock);
    flmr->nrefs++;
    pthread_mutex_unlock(&flmr->lock);
    *mhandle = flmr;
    return ncclSuccess;
  }

  // register base_addr, base_size 
  INFO(NCCL_NET, "FuseLink memregion CREATE");
  FuseLinkMemRegion *flmr = new FuseLinkMemRegion();
  *mhandle = flmr;
  flmr->start_addr = (uintptr_t) base_addr;
  if (type == NCCL_PTR_CUDA) {
    flmr->dev = gpu_id;
  } else {
    flmr->dev = -1;
  }
  INFO(NCCL_NET, "Fuselink memregion init data %p size %zu base_addr %p base_size %zu gpu_id %d is_cuda_mem %d", \
     data, size, base_addr, base_size, gpu_id, type == NCCL_PTR_CUDA);
  FuseLinkMemRegionInit(NGPUs, ncclNIbDevs, NIC2GPU, base_addr, base_size, gpu_id, control_comm->dev_, flmr, type == NCCL_PTR_CUDA);
  INFO(NCCL_NET, "Fuselink memregion init done, dev %d", dev);
  ncclResult_t res = FuseLinkRegMr(flmr, dev, offset, fd);


  addr2flmr[base_addr] = flmr;
  pthread_mutex_unlock(&addr2flmr_lock);

  return ncclSuccess;
}

ncclResult_t ncclIbRegMr(void* comm, void* data, int size, int type, void** mhandle) {
  // printf("ncclIbRegMr: data %p size %d type %d\n", data, size, type);
  return ncclIbRegMrDmaBuf(comm, data, (size_t)size, type, 0ULL, -1, mhandle);
}

ncclResult_t FuseLinkDeregMr(FuseLinkMemRegion* flmr) {
  pthread_mutex_lock(&flmr->lock);
  flmr->nrefs--;
  if (flmr->nrefs > 0) {
    pthread_mutex_unlock(&flmr->lock);
    return ncclSuccess;
  }
  assert(flmr->nrefs == 0);
  // dereg all mrs
  for (int i = 0; i < ncclNIbDevs; i++) {
    if (flmr->mr[i] != NULL) {
      NCCLCHECK(wrap_ibv_dereg_mr(flmr->mr[i]));
      flmr->mr[i] = NULL;
    }
  }
  pthread_mutex_lock(&addr2flmr_lock);
  addr2flmr.erase((void *) flmr->start_addr);
  pthread_mutex_unlock(&addr2flmr_lock);
  pthread_mutex_unlock(&flmr->lock);
  // release all memory handles
  if (flmr->dev != -1) { // gpu memory
    for (int i = 0; i < ncclNIbDevs; i++) {
      if (flmr->addr[i] != 0) {
        INFO(NCCL_NET, "FuseLink memregion DESTROY: unmap and free addr %p size %zu, nic2gpu %d dev %d target nic %d", \
           flmr->addr[i], flmr->sz, flmr->nic2gpu[i], flmr->dev, flmr->nic_dev);
        MYCUCHECK(cuMemUnmap(flmr->addr[i], flmr->sz));
        MYCUCHECK(cuMemAddressFree(flmr->addr[i], flmr->sz));
        if (flmr->nic2gpu[i] != flmr->dev) {
          MYCUCHECK(cuMemRelease(flmr->hdl[i]));
        }
      }
    }
  }
  pthread_mutex_destroy(&flmr->lock);
  delete flmr;
  return ncclSuccess;
}

ncclResult_t ncclIbDeregMr(void* comm, void* mhandle) {
  struct ncclIbVerbs* verbs = (struct ncclIbVerbs*)comm;
  FuseLinkMemRegion* flmr = (FuseLinkMemRegion*) mhandle;
  return FuseLinkDeregMr(flmr);
}

NCCL_PARAM(IbSplitDataOnQps, "IB_SPLIT_DATA_ON_QPS", 1);

ncclResult_t ncclIbMultiSend(struct ncclIbSendComm* comm, int slot) {
  struct ncclIbRequest** reqs = comm->fifoReqs[slot];
  volatile struct ncclIbSendFifo* slots = comm->fifo[slot];
  int nreqs = slots[0].nreqs;
  if (nreqs > NCCL_NET_IB_MAX_RECVS) return ncclInternalError;

  uint64_t wr_id = 0ULL;

  struct FuseLinkComm* data_comm = comm->data_comm;

  for (int r=0; r<nreqs; r++) {
    struct ibv_send_wr* wr = comm->wrs+r;
    memset(wr, 0, sizeof(struct ibv_send_wr));

    struct ibv_sge* sge = comm->sges+r;
    sge->addr=(uintptr_t)reqs[r]->send.data;
    sge->lkey=reqs[r]->send.lkey;

    wr->opcode = IBV_WR_RDMA_WRITE;
    wr->send_flags = 0;
    wr->wr.rdma.remote_addr = slots[r].addr;
    wr->wr.rdma.rkey = slots[r].rkey;

    wr->next = wr+1;
    wr_id += (reqs[r] - comm->reqs) << (r*8);
  }

  // Write size as immediate data. In the case of multi-send, only write
  // 0 or 1 as size to indicate whether there was data sent or received.
  uint32_t immData = 0;
  /** UNET
   * change to request offset
   */
  // if (nreqs == 1) { // cached free NICs
  //   if ( (comm->n_finished + 1) % N_FINISHED_BATCH == 0) {
  //     immData = fuselink_conn_manager->refreshTxUsage(); 
  //   } else {
  //     immData = fuselink_conn_manager->getTxUsage(); // cached.
  //   }
  // } else {
  //   WARN("NET/FuseLink: multi-send not allowed");
  //   return ncclInternalError;
  // }

  struct ibv_send_wr* lastWr = comm->wrs+nreqs-1;
  if (nreqs > 1 || (comm->ar && reqs[0]->send.size > ncclParamIbArThreshold())) {
    // When using ADAPTIVE_ROUTING, send the bulk of the data first as an
    // RDMA_WRITE, then a 0-byte RDMA_WRITE_WITH_IMM to trigger a remote
    // completion.
    lastWr++;
    memset(lastWr, 0, sizeof(struct ibv_send_wr));
  }
  lastWr->wr_id = wr_id;
  lastWr->opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
  lastWr->imm_data = immData;
  lastWr->next = NULL;
  lastWr->send_flags = IBV_SEND_SIGNALED;

  // Multi-QP: make sure IB writes are multiples of 128B so that LL and LL128 protocols still work
  const int align = 128;
  const int nqps = 1;
  for (int q=0; q<nqps; q++) {
    for (int r=0; r<nreqs; r++) {
      int chunkSize = DIVUP(DIVUP(reqs[r]->send.size, nqps), align) * align;
      int length = std::min(reqs[r]->send.size-reqs[r]->send.offset, chunkSize);
      if (length <= 0) {
        comm->wrs[r].sg_list = NULL;
        comm->wrs[r].num_sge = 0;
      } else {
        comm->sges[r].length = length;
        comm->wrs[r].sg_list = comm->sges+r;
        comm->wrs[r].num_sge = 1;
      }
    }
    struct ibv_send_wr* bad_wr;
    // INFO(NCCL_INIT, "multisend %p dev %d sge addr %llx size %ld raddr %llx lkey %x rkey %x", comm, data_comm->dev_, (unsigned long long)comm->wrs[0].sg_list->addr, \
    //  comm->wrs[0].sg_list->length, comm->wrs[0].wr.rdma.remote_addr, comm->wrs[0].sg_list->lkey, comm->wrs[0].wr.rdma.rkey);
    NCCLCHECK(wrap_ibv_post_send(data_comm->qp, comm->wrs, &bad_wr));

    for (int r=0; r<nreqs; r++) {
      int chunkSize = DIVUP(DIVUP(reqs[r]->send.size, nqps), align) * align;
      reqs[r]->send.offset += chunkSize;
      comm->sges[r].addr += chunkSize;
      comm->wrs[r].wr.rdma.remote_addr += chunkSize;
    }
  }

  return ncclSuccess;
}

ncclResult_t ncclIbIsend(void* sendComm, void* data, int size, int tag, void* mhandle, void** request) {
  struct ncclIbSendComm* comm = (struct ncclIbSendComm*)sendComm;
  if (comm->ready == 0) { WARN("NET/FuseLink: ncclIbIsend() called when comm->ready == 0"); return ncclInternalError; }
  if (comm->ready == 0) { *request = NULL; return ncclSuccess; }
  // printf("size: %d\n", size);

  // Wait for the receiver to have posted the corresponding receive
  int nreqs = 0;
  volatile struct ncclIbSendFifo* slots;

  int slot = (comm->fifoHead)%MAX_REQUESTS;
  struct ncclIbRequest** reqs = comm->fifoReqs[slot];
  slots = comm->fifo[slot];
  uint64_t idx = comm->fifoHead+1;
  if (slots[0].idx != idx) { *request = NULL; return ncclSuccess; }
  nreqs = slots[0].nreqs;
  // Wait until all data has arrived
  for (int r=1; r<nreqs; r++) while(slots[r].idx != idx);
  __sync_synchronize(); // order the nreqsPtr load against tag/rkey/addr loads below


  // check if initialized
  // if (get_addr_abs((uintptr_t) request, comm->request_addr) > UPDATE_HACKING_THRESHOLD) {
  //   struct ncclProxySubArgsForHacking* sub = (struct ncclProxySubArgsForHacking*) \
  //    ((uintptr_t) request - offsetof(struct ncclProxySubArgsForHacking, requests));
  //   comm->posted = &sub->posted; // posted to gpu
  //   comm->received = &sub->received;
  //   comm->flushed = &sub->flushed;
  //   comm->transmitted = &sub->transmitted; // send to NIC
  //   comm->done = &sub->done; // send successfully
  //   comm->nsteps = &sub->nsteps; // number of steps
  //   comm->request_addr = (uintptr_t) request;
  // } 
  // INFO(NCCL_INIT, "isend %p posted %d received %d flushed %d transmitted %d done %d", \
  //   comm->posted, *comm->posted, *comm->received, *comm->flushed, *comm->transmitted, *comm->done);
  // ..
  // select the proper comm
  // INFO(NCCL_INIT|NCCL_NET, "fuselink_offset %d", slots[0].fuselink_offset);
  // int fuselink_offset = slots[0].fuselink_offset;
  // if (fuselink_offset == -1) {
  //   comm->data_comm = comm;
  // } else {
  //   void* data_comm = fuselink_conn_manager->bidTxChannel(comm->channelId, fuselink_offset);
  //   if (data_comm == NULL) {// if we have to use the original nic
  //     data_comm = (void *) comm;
  //   }
  //   comm->data_comm = (struct ncclIbSendComm *) data_comm;
  // }

  for (int r=0; r<nreqs; r++) {
    if (reqs[r] != NULL || slots[r].tag != tag) continue;

    // Sanity checks to catch user collective call count/size mismatches
    if (size > slots[r].size) {
      char line[SOCKET_NAME_MAXLEN + 1];
      union ncclSocketAddress addr;
      ncclSocketGetAddr(&comm->sock, &addr);
      WARN("NET/FuseLink : req %d/%d tag %x peer %s collective mismatch error, local size %d remote size %d",
        r, nreqs, tag, ncclSocketToString(&addr, line), size, slots[r].size);
      return ncclInvalidUsage;
    } // plus any potential programming errors
    else if (slots[r].size < 0 || slots[r].addr == 0 || slots[r].rkey == 0) {
      char line[SOCKET_NAME_MAXLEN + 1];
      union ncclSocketAddress addr;
      ncclSocketGetAddr(&comm->sock, &addr);
      WARN("NET/FuseLink : req %d/%d tag %x peer %s posted incorrect receive info: size %d addr %lx rkey %x",
        r, nreqs, tag, ncclSocketToString(&addr, line), slots[r].size, slots[r].addr, slots[r].rkey);
      return ncclInternalError;
    }
    struct ncclIbRequest* req;
    NCCLCHECK(ncclIbGetRequest(comm->reqs, &req));
    req->sock = &comm->sock;
    struct FuseLinkMemRegion* flmr = (struct FuseLinkMemRegion*) mhandle;
    req->type = NCCL_NET_IB_REQ_SEND;
    req->control_comm = comm->control_comm;
    req->data_comm = comm->data_comm;
    req->nreqs = nreqs;
    req->send.size = size;
    // convert data to fuselink address
    INFO(NCCL_NET, "data %p, on dev %d, comm_dev %d base_addr %p regaddr %p", \
       data, flmr->dev, comm->data_comm->dev_, flmr->start_addr, flmr->addr[flmr->dev]);
    req->send.data = (void *) (flmr->addr[comm->data_comm->dev_] + data - flmr->start_addr);
    req->send.lkey = flmr->mr[comm->data_comm->dev_]->lkey;
    req->send.offset = 0;
    req->events = 1;
    *request = reqs[r] = req;

    // If this is a multi-recv, send only when all requests have matched.
    for (int r=0; r<nreqs; r++) {
      if (reqs[r] == NULL) return ncclSuccess;
    }

    TIME_START(0);
    NCCLCHECK(ncclIbMultiSend(comm, slot));

    // Clear slots[0]->nreqs, as well as other fields to help debugging and sanity checks
    memset((void*)slots, 0, sizeof(struct ncclIbSendFifo));
    memset(reqs, 0, NCCL_NET_IB_MAX_RECVS*sizeof(struct ncclIbRequest*));
    comm->fifoHead++;
    TIME_STOP(0);
    return ncclSuccess;
  }

  *request = NULL;
  return ncclSuccess;
}

ncclResult_t ncclIbPostFifo(struct ncclIbRecvComm* comm, int n, void** data, int* sizes, int* tags, void** mhandles, struct ncclIbRequest* req, int fuselink_offset) {
  struct ibv_send_wr wr;
  memset(&wr, 0, sizeof(wr));

  int slot = comm->remFifo.fifoTail%MAX_REQUESTS;
  struct ncclIbSendFifo* localElem = comm->remFifo.elems[slot];

  for (int i=0; i<n; i++) {
    struct FuseLinkMemRegion* flmr = (struct FuseLinkMemRegion*) mhandles[i];
    // convert data to fuselink address
    localElem[i].addr = (uint64_t) (flmr->addr[comm->data_comm->dev_] + data[i] - flmr->start_addr);
    struct ibv_mr* mr = flmr->mr[comm->data_comm->dev_];
    localElem[i].rkey = mr->rkey;
    localElem[i].nreqs = n;
    localElem[i].size = sizes[i]; // Sanity/Debugging
    localElem[i].tag = tags[i];
    localElem[i].idx = comm->remFifo.fifoTail+1;
    localElem[i].fuselink_offset = fuselink_offset;
    /**
     * UNET
     * add/change to request offset
     */
  }

  wr.wr.rdma.remote_addr = comm->remFifo.addr + slot*NCCL_NET_IB_MAX_RECVS*sizeof(struct ncclIbSendFifo);
  wr.wr.rdma.rkey = comm->remFifo.rkey;
  comm->remFifo.sge.addr = (uint64_t)localElem;
  comm->remFifo.sge.length = n*sizeof(struct ncclIbSendFifo);
  wr.sg_list = &comm->remFifo.sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_RDMA_WRITE;
  wr.send_flags = comm->remFifo.flags; // IBV_SEND_INLINE

  // We need to occasionally post a request with the IBV_SEND_SIGNALED flag, otherwise
  // the send queue will never empty.
  //
  // From https://www.rdmamojo.com/2014/06/30/working-unsignaled-completions/
  // "How to use Unsignaled Completion?" / "Gotchas and Pitfalls"
  // All posted Send Requested, Signaled and Unsignaled, are considered outstanding until
  // a Work Completion that they, or Send Requests that were posted after them, was polled
  // from the Completion Queue associated with the Send Queue. This means if one works with
  // a Queue Pair that was configured to work with Unsignaled Completions, he must make
  // sure that occasionally (before the Send Queue is full with outstanding Send Requests)
  // a Send Request that generate Work Completion will be posted.
  //
  // Not following this rule may lead to a case that the Send Queue is full with Send
  // Requests that won't generate Work Completion:
  //
  //  - The Send Queue is full, so no new Send Requests can be posted to it
  //  - The Send Queue can't be emptied, since no Work Completion can be generated anymore
  //    (the reason is that no Work Completion, that can generate Work Completion that
  //    polling it will empty the Send Queue, can be posted)
  //  - The status of all posted Send Request is considered unknown
  //
  if (slot == 0) {
    wr.send_flags |= IBV_SEND_SIGNALED;
    wr.wr_id = req - comm->reqs;
    req->events++;
  }

  struct ibv_send_wr* bad_wr;
  NCCLCHECK(wrap_ibv_post_send(comm->control_comm->qp, &wr, &bad_wr));
  comm->remFifo.fifoTail++;

  return ncclSuccess;
}

ncclResult_t ncclIbIrecv(void* recvComm, int n, void** data, int* sizes, int* tags, void** mhandles, void** request) {
  struct ncclIbRecvComm* comm = (struct ncclIbRecvComm*)recvComm;
  if (comm->ready == 0) { WARN("NET/FuseLink: ncclIbIrecv() called when comm->ready == 0"); return ncclInternalError; }
  if (comm->ready == 0) { *request = NULL; return ncclSuccess; }
  if (n > NCCL_NET_IB_MAX_RECVS) return ncclInternalError;

  // need to hack upper layer info
  // if (get_addr_abs((uintptr_t) request, comm->request_addr) > UPDATE_HACKING_THRESHOLD) {
  //   struct ncclProxySubArgsForHacking* sub = (struct ncclProxySubArgsForHacking*) \
  //    ((uintptr_t) request - offsetof(struct ncclProxySubArgsForHacking, requests));
  //   comm->posted = &sub->posted;
  //   comm->received = &sub->received;
  //   comm->flushed = &sub->flushed;
  //   comm->transmitted = &sub->transmitted;
  //   comm->done = &sub->done;
  //   comm->request_addr = (uintptr_t) request;
  //   comm->nsteps = &sub->nsteps;
  // }
  // INFO(NCCL_INIT, "ncclIbIrecv: posted %d received %d flushed %d transmitted %d done %d nsteps %d",
  //   *comm->posted, *comm->received, *comm->flushed, *comm->transmitted, *comm->done, *comm->nsteps);

  // if (comm->n_finished % N_FINISHED_BATCH == 0) {
  //   // update side comm
  //   // INFO(NCCL_INIT, "UPDATING %p", fuselink_conn_manager);
  //   // make sure that all recved data has been consumed by the upper layer
  //   if (*(comm->received) == *(comm->done)) {
  //     INFO(NCCL_INIT, "ncclIbIrecv: all recved data has been consumed by the upper layer");
  //   } else {
  //     INFO(NCCL_INIT, "ncclIbIrecv: recved %d done %d", *(comm->received), *(comm->done));
  //     return ncclSuccess;
  //   }
  // }
  
  struct ncclIbRequest* req;
  NCCLCHECK(ncclIbGetRequest(comm->reqs, &req));
  req->type = NCCL_NET_IB_REQ_RECV;
  req->control_comm = comm->control_comm;
  req->data_comm = comm->data_comm;
  req->sock = &comm->sock;
  req->nreqs = n;
  if (comm->gidInfo.link_layer == IBV_LINK_LAYER_ETHERNET) req->gidInfo = &comm->gidInfo;
  for (int i=0; i<n; i++) req->recv.sizes[i] = 0;

  struct ibv_recv_wr wr;
  memset(&wr, 0, sizeof(wr));
  wr.wr_id = req - comm->reqs;

  wr.sg_list = NULL;
  wr.num_sge = 0;

  TIME_START(1);
  struct ibv_qp* qp = comm->data_comm->qp;
  struct ibv_recv_wr* bad_wr;
  NCCLCHECK(wrap_ibv_post_recv(qp, &wr, &bad_wr));
  
  TIME_STOP(1);
  req->events = 1;

  *request = req;
  
  // INFO(NCCL_INIT, "recv %p posted %d received %d flushed %d transmitted %d done %d", \
  //    comm->posted, *comm->posted, *comm->received, *comm->flushed, *comm->transmitted, *comm->done);
  
  // req->posted = comm->posted;
  // req->received = comm->received;
  // req->flushed = comm->flushed;
  // req->transmitted = comm->transmitted;
  // req->done = comm->done;
  // req->nsteps = comm->nsteps;

  // Post to FIFO to notify sender
  TIME_START(2);
  // comm is actually the fifo_comm
  NCCLCHECK(ncclIbPostFifo(comm, n, data, sizes, tags, mhandles, req, comm->fuselink_offset));
  TIME_STOP(2);

  return ncclSuccess;
}

ncclResult_t ncclIbIflush(void* recvComm, int n, void** data, int* sizes, void** mhandles, void** request) {
  struct ncclIbRecvComm* comm = (struct ncclIbRecvComm*)recvComm;
  int last = -1;
  for (int i=0; i<n; i++) if (sizes[i]) last = i;
  if (comm->gpuFlush.enabled == 0 || last == -1) return ncclSuccess;

  // Only flush once using the last non-zero receive
  struct ncclIbRequest* req;
  NCCLCHECK(ncclIbGetRequest(comm->reqs, &req));
  req->type = NCCL_NET_IB_REQ_FLUSH;
  req->sock = &comm->sock;
  struct ibv_mr* mr = (struct ibv_mr*)mhandles[last];

  struct ibv_send_wr wr;
  memset(&wr, 0, sizeof(wr));
  wr.wr_id = req - comm->reqs;

  wr.wr.rdma.remote_addr = (uint64_t)data[last];
  wr.wr.rdma.rkey = mr->rkey;
  wr.sg_list = &comm->gpuFlush.sge;
  wr.num_sge = 1;
  wr.opcode = IBV_WR_RDMA_READ;
  wr.send_flags = IBV_SEND_SIGNALED;

  TIME_START(4);
  struct ibv_send_wr* bad_wr;
  NCCLCHECK(wrap_ibv_post_send(comm->gpuFlush.qp, &wr, &bad_wr));
  TIME_STOP(4);

  *request = req;
  return ncclSuccess;
}

ncclResult_t ncclIbTest(void* request, int* done, int* sizes) {
  struct ncclIbRequest *r = (struct ncclIbRequest*)request;
  *done = 0;
  // INFO(NCCL_INIT, "test posted %d received %d flushed %d transmitted %d done %d nsteps %d",
  //   *r->posted, *r->received, *r->flushed, *r->transmitted, *r->done, *r->nsteps);
  while (1) {
    if (r->events == 0) {
      *done = 1;
      // todo: update nic activities.
      if (sizes && r->type == NCCL_NET_IB_REQ_RECV) {
        for (int i=0; i<r->nreqs; i++) sizes[i] = r->recv.sizes[i];
      }
      NCCLCHECK(ncclIbFreeRequest(r));
      return ncclSuccess;
    }

    int wrDone1 = 0, wrDone2 = 0;
    struct ibv_wc wcs1[4], wcs2[4];
    TIME_START(3);
    // caution: r->verbs with qp must match

    FuseLinkComm* control_comm = (FuseLinkComm*)r->control_comm;
    FuseLinkComm* data_comm = (FuseLinkComm*)r->data_comm;

    NCCLCHECK(wrap_ibv_poll_cq(control_comm->cq, 4, wcs1, &wrDone1));
    NCCLCHECK(wrap_ibv_poll_cq(data_comm->cq, 4, wcs2, &wrDone2));
    if (wrDone1 == 0 && wrDone2 == 0) { TIME_CANCEL(3); } else { TIME_STOP(3); }
    if (wrDone1 == 0 && wrDone2 == 0) return ncclSuccess;
    // INFO(NCCL_INIT|NCCL_NET, "poll cq on dev %d type %d", r->verbs->dev, r->type);
    // INFO(NCCL_INIT|NCCL_NET, "wrDone1 %d wrDone2 %d", wrDone1, wrDone2);
    for (int w=0; w<wrDone1; w++) {
      struct ibv_wc *wc = wcs1+w;
      if (wc->status != IBV_WC_SUCCESS) {
        char line[SOCKET_NAME_MAXLEN+1];
        union ncclSocketAddress addr;
        ncclSocketGetAddr(r->sock, &addr);
        char localGidString[INET6_ADDRSTRLEN] = "";
        char remoteGidString[INET6_ADDRSTRLEN] = "";
        const char* localGidStr = NULL, *remoteGidStr = NULL;
        if (r->gidInfo) {
            localGidStr = inet_ntop(AF_INET6, &r->gidInfo->localGid, localGidString, sizeof(localGidString));
            remoteGidStr = inet_ntop(AF_INET6, &r->gidInfo->remoteGid, remoteGidString, sizeof(remoteGidString));
        }
        WARN("NET/FuseLink : Got completion from peer %s with error %d, wr_id %ld, dev %d, opcode %d, len %d, vendor err %d (%s)%s%s%s%s",
            ncclSocketToString(&addr, line), wc->status, wc->wr_id, control_comm->dev_, wc->opcode, wc->byte_len, wc->vendor_err, reqTypeStr[r->type],
            localGidStr ?  " localGid ":"", localGidString, remoteGidStr ? " remoteGid ":"", remoteGidString);
        return ncclRemoteError;
      }

      struct ncclIbRequest* req = r->reqs+(wc->wr_id & 0xff);
      if (req->type == NCCL_NET_IB_REQ_SEND) {
        for (int i=0; i<req->nreqs; i++) {
          struct ncclIbRequest* sendReq = r->reqs+((wc->wr_id >> (i*8)) & 0xff);
          if ((sendReq->events <= 0)) return ncclInternalError;
          sendReq->events--;
        }
      } else {
        if (req && wc->opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
          if (req->type != NCCL_NET_IB_REQ_RECV) return ncclInternalError;
          if (req->nreqs > 1) {
            // // In the case of a multi recv, we only set sizes to 0 or 1.
            // for (int i=0; i<req->nreqs; i++) {
            //   req->recv.sizes[i] = (wc->imm_data >> i) & 0x1;
            // }
            // not allowed
            WARN("NET/FuseLink: multi recv not allowed");
            return ncclInternalError;
          } else {
            // cache the imm_data as the mask
            // comm->txAvailable = wc->imm_data;
          }
        }
        req->events--;
        INFO (NCCL_INIT|NCCL_NET, "req->type %d req->events %d", req->type, req->events);
      }
    }
    for (int w=0; w<wrDone2; w++) {
      struct ibv_wc *wc = wcs2+w;
      if (wc->status != IBV_WC_SUCCESS) {
        char line[SOCKET_NAME_MAXLEN+1];
        union ncclSocketAddress addr;
        ncclSocketGetAddr(r->sock, &addr);
        char localGidString[INET6_ADDRSTRLEN] = "";
        char remoteGidString[INET6_ADDRSTRLEN] = "";
        const char* localGidStr = NULL, *remoteGidStr = NULL;
        if (r->gidInfo) {
            localGidStr = inet_ntop(AF_INET6, &r->gidInfo->localGid, localGidString, sizeof(localGidString));
            remoteGidStr = inet_ntop(AF_INET6, &r->gidInfo->remoteGid, remoteGidString, sizeof(remoteGidString));
        }
        WARN("NET/FuseLink : Got completion from peer %s with error %d, wr_id %ld, dev %d, opcode %d, len %d, vendor err %d (%s)%s%s%s%s",
            ncclSocketToString(&addr, line), wc->status, wc->wr_id, data_comm->dev_, wc->opcode, wc->byte_len, wc->vendor_err, reqTypeStr[r->type],
            localGidStr ?  " localGid ":"", localGidString, remoteGidStr ? " remoteGid ":"", remoteGidString);
        return ncclRemoteError;
      }

      struct ncclIbRequest* req = r->reqs+(wc->wr_id & 0xff);
      if (req->type == NCCL_NET_IB_REQ_SEND) {
        for (int i=0; i<req->nreqs; i++) {
          struct ncclIbRequest* sendReq = r->reqs+((wc->wr_id >> (i*8)) & 0xff);
          if ((sendReq->events <= 0)) return ncclInternalError;
          sendReq->events--;
        }
      } else {
        if (req && wc->opcode == IBV_WC_RECV_RDMA_WITH_IMM) {
          if (req->type != NCCL_NET_IB_REQ_RECV) return ncclInternalError;
          if (req->nreqs > 1) {
            // // In the case of a multi recv, we only set sizes to 0 or 1.
            // for (int i=0; i<req->nreqs; i++) {
            //   req->recv.sizes[i] = (wc->imm_data >> i) & 0x1;
            // }
            // not allowed
            WARN("NET/FuseLink: multi recv not allowed");
            return ncclInternalError;
          } else {
            // req->recv.sizes[0] += wc->imm_data;
            // cache the imm_data as the mask
            // comm->txAvailable = wc->imm_data;
          }
        }
        req->events--;
        INFO (NCCL_INIT|NCCL_NET, "req->type %d req->events %d", req->type, req->events);
      }
    }
    
  } // while 1

  // for sending, if we need to change path, we need to wait until all other sending requests in the buffer are done
  // in addition, we need to make sure that the GPU is not writing to the buffer

  // for receiving, if we need to change path, all received data must be consumed
  // and there should not be any outstanding receive requests
}

ncclResult_t FuseLinkCloseSend(FuseLinkComm* sendComm) {
  NCCLCHECK(wrap_ibv_destroy_qp(sendComm->qp));
  NCCLCHECK(wrap_ibv_destroy_cq(sendComm->cq));

  pthread_mutex_lock(&ncclIbDevs[sendComm->dev_].lock);
  if (0 == --ncclIbDevs[sendComm->dev_].pdRefs) {
    NCCLCHECK(wrap_ibv_dealloc_pd(ncclIbDevs[sendComm->dev_].pd));
  }
  pthread_mutex_unlock(&ncclIbDevs[sendComm->dev_].lock);

  return ncclSuccess;
}

ncclResult_t ncclIbCloseSend(void* sendComm) {
  struct ncclIbSendComm* comm = (struct ncclIbSendComm*)sendComm;
  if (comm) {
    int channelId = comm->channelId;
    NCCLCHECK(ncclSocketClose(&comm->sock));
    for (uint i = 0; i < ncclNIbDevs; i++) {
      if (fuselink_send_comm_[channelId][i] != NULL) {
        NCCLCHECK(FuseLinkCloseSend(fuselink_send_comm_[channelId][i]));
      }
    }
    if (comm->fifoMr != NULL) NCCLCHECK(wrap_ibv_dereg_mr(comm->fifoMr));
    free(comm);
  }
  TIME_PRINT("IB");
  return ncclSuccess;
}

ncclResult_t FuseLinkCloseRecv(FuseLinkComm* recvComm) {
  NCCLCHECK(wrap_ibv_destroy_qp(recvComm->qp));
  NCCLCHECK(wrap_ibv_destroy_cq(recvComm->cq));

  pthread_mutex_lock(&ncclIbDevs[recvComm->dev_].lock);
  if (0 == --ncclIbDevs[recvComm->dev_].pdRefs) {
    NCCLCHECK(wrap_ibv_dealloc_pd(ncclIbDevs[recvComm->dev_].pd));
  }
  pthread_mutex_unlock(&ncclIbDevs[recvComm->dev_].lock);

  return ncclSuccess;
}

ncclResult_t ncclIbCloseRecv(void* recvComm) {
  struct ncclIbRecvComm* comm = (struct ncclIbRecvComm*)recvComm;
  if (comm) {
    int channelId = comm->channelId;
    NCCLCHECK(ncclSocketClose(&comm->sock));
    for (uint i = 0; i < ncclNIbDevs; i++) {
      if (fuselink_recv_comm_[channelId][i] != NULL) {
        NCCLCHECK(FuseLinkCloseRecv(fuselink_recv_comm_[channelId][i]));
      }
    }
    if (comm->gpuFlush.enabled) {
      if (comm->gpuFlush.qp != NULL) NCCLCHECK(wrap_ibv_destroy_qp(comm->gpuFlush.qp));
      if (comm->gpuFlush.hostMr != NULL) NCCLCHECK(wrap_ibv_dereg_mr(comm->gpuFlush.hostMr));
    }
    if (comm->remFifo.mr != NULL) NCCLCHECK(wrap_ibv_dereg_mr(comm->remFifo.mr));
    free(comm);
  }
  return ncclSuccess;
}

ncclResult_t ncclIbCloseListen(void* listenComm) {
  struct ncclIbListenComm* comm = (struct ncclIbListenComm*)listenComm;
  if (comm) {
    NCCLCHECK(ncclSocketClose(&comm->sock));
    free(comm);
  }
  return ncclSuccess;
}

ncclNet_t ncclNetPlugin_v7 = {
  "FuseLink",
  ncclIbInit,
  ncclIbDevices,
  ncclIbGetProperties,
  ncclIbListen,
  ncclIbConnect,
  ncclIbAccept,
  ncclIbRegMr,
  ncclIbRegMrDmaBuf,
  ncclIbDeregMr,
  ncclIbIsend,
  ncclIbIrecv,
  ncclIbIflush,
  ncclIbTest,
  ncclIbCloseSend,
  ncclIbCloseRecv,
  ncclIbCloseListen,
  NULL /* getDeviceMr */,
  NULL /* irecvConsumed */
};
ncclNet_t ncclNetPlugin_v6 = ncclNetPlugin_v7;
