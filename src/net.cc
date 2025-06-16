#include "net.h"
#include <cstdint>
#include <cstdio>
#include <infiniband/verbs.h>


void IbTest(struct CpuTask **task, int num_tasks, int *mask) {
    int wrDone = 0;
    struct ibv_wc wcs[4];
    wrDone = ibv_poll_cq(task[0]->cq, 4, wcs);

    for (int w = 0; w < wrDone; w++) {
        struct ibv_wc *wc = &wcs[w];
        if (wc->status != IBV_WC_SUCCESS) {
            fprintf(stderr, "IBV_WC error: %s\n", ibv_wc_status_str(wc->status));
            continue; // Skip this wc if there is an error
        }
        for (int i = 0; i < num_tasks; i++) {
            if ((uintptr_t)wc->wr_id == (uintptr_t)task[i]) {
                if (wc->status == IBV_WC_SUCCESS) {
                    task[i]->transmit_complete_flag = 1;
                    mask[i] = 1;
                } else {
                    fprintf(stderr, "IBV_WC error: %s\n", ibv_wc_status_str(wc->status));
                    task[i]->transmit_complete_flag = -1; // Indicate error
                }
            }
        }
    }
}

void IbSend(struct CpuTask *task, struct IbSendComm *sendComm) {
    int slot = sendComm->fifoHead % MAX_REQUESTS;
    uint64_t idx = sendComm->fifoHead + 1;
    struct SendFifo *localElem = &sendComm->fifo[slot];
    while(localElem->idx != idx) ;
    __sync_synchronize();

    struct ibv_send_wr wr = {};
    struct ibv_send_wr siganl_wr = {};
    struct ibv_send_wr *bad_wr;
    struct ibv_sge sge = {};

    sge.addr = (uintptr_t)task->buffer;
    sge.length = task->buffer_size;
    sge.lkey = task->mr->lkey;

    wr.wr.rdma.remote_addr = localElem->addr;
    wr.wr.rdma.rkey = localElem->rkey;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = 0;
    wr.next = &siganl_wr;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    siganl_wr.wr_id = (uintptr_t)task;
    siganl_wr.opcode = IBV_WR_SEND_WITH_IMM;
    siganl_wr.imm_data = task->buffer_size;
    siganl_wr.next = NULL;
    siganl_wr.send_flags = IBV_SEND_SIGNALED;

    int ret = ibv_post_send(task->qp, &wr, &bad_wr);
    if (ret) {
        fprintf(stderr, "Failed to post send: %d\n", ret);
        return; // Handle error appropriately
    }

    memset((void*)localElem, 0, sizeof(struct SendFifo));
    sendComm->fifoHead++;
}

void IbRecv(struct CpuTask* task, struct RemFifo* remFifo) {
    struct ibv_send_wr wr = {};
    struct ibv_send_wr* bad_wr;

    const int slot = remFifo->fifoTail % MAX_REQUESTS;
    struct SendFifo* localElem = &remFifo->elems[slot];

    localElem->addr = (uint64_t)task->buffer;
    localElem->rkey = task->mr->rkey;
    localElem->size = task->buffer_size; // Sanity/Debugging
    localElem->idx = remFifo->fifoTail + 1; // idx is used to track the order of requests

    wr.wr_id = (uintptr_t)task;
    wr.wr.rdma.remote_addr = remFifo->addr + slot * sizeof(struct SendFifo);
    wr.wr.rdma.rkey = remFifo->rkey;
    remFifo->sge.addr = (uintptr_t)localElem;
    remFifo->sge.length = sizeof(struct SendFifo);
    wr.sg_list = &remFifo->sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_INLINE;

    int ret = ibv_post_send(task->qp, &wr, &bad_wr);
    if (ret) {
        fprintf(stderr, "Failed to post send: %d\n", ret);
        return; // Handle error appropriately
    }
    remFifo->fifoTail++;
    // lkey of sge should be the same for every wr, pls configured it at init
}