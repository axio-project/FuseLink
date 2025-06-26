#ifndef NET_H
#define NET_H

#include <rdma/rdma_verbs.h>
#include "task.h"

#define MAX_REQUESTS 8

struct SendFifo {
  uint64_t addr;
  int      size;
  uint32_t rkey;
  uint64_t idx;
};

struct IbSendComm {
  struct SendFifo fifo[MAX_REQUESTS];
  uint64_t fifoHead;
  struct ibv_mr* fifoMr; // send the fifo mr rkey to remote during connection setup
};

struct RemFifo {
  struct SendFifo elems[MAX_REQUESTS];
  uint64_t fifoTail;
  uint64_t addr;
  uint32_t rkey; // rkey of the remote fifo, sent during connection setup
  struct ibv_mr* mr; // Memory region for the local elems
  struct ibv_sge sge; // lkey should be mr's lkey
};

void IbSend(struct CpuTask *task, struct IbSendComm *sendComm);
void IbRecv(struct CpuTask* task, struct RemFifo* remFifo);
void IbTest(struct CpuTask **task, int num_tasks, int *mask);

#endif