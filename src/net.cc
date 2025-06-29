#include "net.h"
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <infiniband/verbs.h>
#include <sys/types.h>


void IbTest(uint64_t *wr_ids, int num_tasks, int *mask, struct ibv_cq *cq) {
    int wrDone = 0;
    struct ibv_wc wcs[4];
    wrDone = ibv_poll_cq(cq, 4, wcs);

    for (int w = 0; w < wrDone; w++) {
        struct ibv_wc *wc = &wcs[w];
        if (wc->status != IBV_WC_SUCCESS) {
            fprintf(stderr, "IBV_WC error: %s\n", ibv_wc_status_str(wc->status));
            continue; // Skip this wc if there is an error
        }
        for (int i = 0; i < num_tasks; i++) {
            if ((uintptr_t)wc->wr_id == wr_ids[i]) {
                if (wc->status == IBV_WC_SUCCESS) {
                    mask[i] = 1;
                } else {
                    fprintf(stderr, "IBV_WC error: %s\n", ibv_wc_status_str(wc->status));
                }
            }
        }
    }
}

void IbSend(void *buffer, size_t buffer_size, struct ibv_mr *mr, struct ibv_qp* qp, struct IbSendComm *sendComm, uint64_t wr_id) {
    int slot = sendComm->fifoHead % MAX_REQUESTS;
    uint64_t idx = sendComm->fifoHead + 1;
    struct SendFifo *localElem = &sendComm->fifo[slot];
    while(localElem->idx != idx) ;
    __sync_synchronize();

    struct ibv_send_wr wr = {};
    struct ibv_send_wr siganl_wr = {};
    struct ibv_send_wr *bad_wr;
    struct ibv_sge sge = {};

    sge.addr = (uintptr_t)buffer;
    sge.length = buffer_size;
    sge.lkey = mr->lkey;

    wr.wr.rdma.remote_addr = localElem->addr;
    wr.wr.rdma.rkey = localElem->rkey;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = 0;
    wr.next = &siganl_wr;
    wr.sg_list = &sge;
    wr.num_sge = 1;

    siganl_wr.wr_id = wr_id;
    siganl_wr.opcode = IBV_WR_SEND_WITH_IMM;
    siganl_wr.imm_data = buffer_size;
    siganl_wr.next = NULL;
    siganl_wr.send_flags = IBV_SEND_SIGNALED;

    int ret = ibv_post_send(qp, &wr, &bad_wr);
    if (ret) {
        fprintf(stderr, "Failed to post send: %d\n", ret);
        return; // Handle error appropriately
    }

    memset((void*)localElem, 0, sizeof(struct SendFifo));
    sendComm->fifoHead++;
}

void IbRecv(void* buffer, size_t buffer_size, struct ibv_mr *mr, struct ibv_qp *qp, struct RemFifo* remFifo, uint64_t wr_id) {
    struct ibv_send_wr wr = {};
    struct ibv_send_wr* bad_wr;

    const int slot = remFifo->fifoTail % MAX_REQUESTS;
    struct SendFifo* localElem = &remFifo->elems[slot];

    struct ibv_recv_wr recv_wr;
    struct ibv_recv_wr *bad_recv_wr;
    memset(&recv_wr, 0, sizeof(recv_wr));
    recv_wr.wr_id = wr_id;
    recv_wr.sg_list = NULL;
    recv_wr.num_sge = 0;

    int ret = ibv_post_recv(qp, &recv_wr, &bad_recv_wr);
    if (ret) {
        fprintf(stderr, "Failed to post recv: %d\n", ret);
        return; // Handle error appropriately
    }

    localElem->addr = (uint64_t)buffer;
    localElem->rkey = mr->rkey;
    localElem->size = buffer_size;
    localElem->idx = remFifo->fifoTail + 1;

    wr.wr_id = wr_id;
    wr.wr.rdma.remote_addr = remFifo->addr + slot * sizeof(struct SendFifo);
    wr.wr.rdma.rkey = remFifo->rkey;
    remFifo->sge.addr = (uintptr_t)localElem;
    remFifo->sge.length = sizeof(struct SendFifo);
    remFifo->sge.lkey = remFifo->mr->lkey;
    wr.sg_list = &remFifo->sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_INLINE;

    ret = ibv_post_send(qp, &wr, &bad_wr);
    if (ret) {
        fprintf(stderr, "Failed to post send: %d\n", ret);
        return; // Handle error appropriately
    }
    remFifo->fifoTail++;
    // lkey of sge should be the same for every wr, pls configured it at init
}