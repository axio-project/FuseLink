#include <cstdint>
#include <infiniband/verbs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include "task.h"
#include "net.h"
#define PORT 18515
#define MSG_SIZE 4096

int main() {
    struct ibv_device **dev_list;
    struct ibv_context *ctx;
    struct ibv_pd *pd;
    struct ibv_mr *mr;
    struct ibv_cq *cq;
    struct ibv_qp *qp;
    struct ibv_qp_init_attr qp_init_attr;
    char *buf;
    dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        fprintf(stderr, "Failed to get RDMA devices.\n");
        return EXIT_FAILURE;
    }
    printf("Available RDMA devices:\n");
    for (int i = 0; dev_list[i]; ++i) {
        struct ibv_device *dev = dev_list[i];
        printf("Device %d: %s\n", i, ibv_get_device_name(dev));
    }
    ctx = ibv_open_device(dev_list[0]);
    if (!ctx) {
        fprintf(stderr, "Failed to open RDMA device.\n");
        return EXIT_FAILURE;
    }
    pd = ibv_alloc_pd(ctx);
    if (!pd) {
        fprintf(stderr, "Failed to allocate protection domain.\n");
        return EXIT_FAILURE;
    }
    buf = (char*)malloc(MSG_SIZE);
    if (!buf) {
        fprintf(stderr, "Failed to allocate memory buffer.\n");
        return EXIT_FAILURE;
    }
    mr = ibv_reg_mr(pd, buf, MSG_SIZE, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!mr) {
        fprintf(stderr, "Failed to register memory region.\n");
        free(buf);
        return EXIT_FAILURE;
    }
    cq = ibv_create_cq(ctx, 10, NULL, NULL, 0);
    if (!cq) {
        fprintf(stderr, "Failed to create completion queue.\n");
        ibv_dereg_mr(mr);
        free(buf);
        return EXIT_FAILURE;
    }
    memset(&qp_init_attr, 0, sizeof(qp_init_attr));
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
        ibv_dereg_mr(mr);
        free(buf);
        return EXIT_FAILURE;
    }
    struct ibv_qp_attr qp_attr;
    memset(&qp_attr, 0, sizeof(qp_attr));
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.port_num = 1;
    qp_attr.pkey_index = 0;
    qp_attr.qp_access_flags = IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_LOCAL_WRITE;
    if (ibv_modify_qp(qp, &qp_attr,
        IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS)) {
        fprintf(stderr, "Failed to modify QP to INIT\n");
        exit(EXIT_FAILURE);
    } else {
        printf("QP state changed from RESET to INIT.\n");
    }
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(PORT);
    addr.sin_addr.s_addr = INADDR_ANY;
    int ret = bind(sockfd, (struct sockaddr*)&addr, sizeof(addr));
    if (ret < 0) {
        perror("bind");
        exit(EXIT_FAILURE);
    }
    listen(sockfd, 1);
    int connfd = accept(sockfd, NULL, NULL);

    // 使用GID而不是LID进行连接信息交换
    struct {
        uint32_t qp_num;
        union ibv_gid gid;
        uint64_t addr;
        uint32_t rkey;
    } local, remote;

    // 获取本地GID（RoCE必须用GID，通常index为0）
    if (ibv_query_gid(ctx, 1, 0, &local.gid)) {
        fprintf(stderr, "ibv_query_gid failed\n");
        exit(EXIT_FAILURE);
    }
    local.qp_num = qp->qp_num;
    local.addr = (uintptr_t)buf;
    local.rkey = mr->rkey;

    // 先写本地，再读远端
    write(connfd, &local, sizeof(local));
    read(connfd, &remote, sizeof(remote));
    printf("Local QP: num=%u, gid=%s, addr=%lu, rkey=%u\n",
           local.qp_num, inet_ntoa(*(struct in_addr *)&local.gid.global.subnet_prefix),
           local.addr, local.rkey);
    printf("Remote QP: num=%u, gid=%s, addr=%lu, rkey=%u\n",
           remote.qp_num, inet_ntoa(*(struct in_addr *)&remote.gid.global.subnet_prefix),
           remote.addr, remote.rkey);

    // RTR
    struct ibv_qp_attr attr = {};
    attr.qp_state = IBV_QPS_RTR;
    attr.path_mtu = IBV_MTU_1024;
    attr.rq_psn = 0;
    attr.dest_qp_num = remote.qp_num;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer = 12;
    attr.ah_attr.is_global = 1;
    attr.ah_attr.dlid = 0; // RoCE下无LID
    attr.ah_attr.port_num = 1;
    attr.ah_attr.grh.dgid = remote.gid;
    attr.ah_attr.grh.sgid_index = 0;
    attr.ah_attr.grh.hop_limit = 1;

    if (ibv_modify_qp(qp, &attr,
        IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU |
        IBV_QP_DEST_QPN | IBV_QP_RQ_PSN |
        IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER)) {
        fprintf(stderr, "Failed to modify QP to RTR\n");
        exit(EXIT_FAILURE);
    } else {
        printf("QP state changed from INIT to RTR.\n");
    }

    // RTS
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
        printf("QP state changed from RTR to RTS.\n");
    }

    printf("RDMA server (RoCE) ready.\n");

    // 构造要发送的数据
    strcpy(buf, "Hello RDMA from server!");

    IbSendComm sendComm;
    memset(&sendComm, 0, sizeof(sendComm));
    sendComm.fifoMr = ibv_reg_mr(pd, &sendComm.fifo, sizeof(sendComm.fifo), IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    if (!sendComm.fifoMr) {
        fprintf(stderr, "Failed to register send FIFO memory region.\n");
        return EXIT_FAILURE;
    }

    // pass  the send FIFO MR to the remote
    local.addr = (uintptr_t)&sendComm.fifo;
    local.rkey = sendComm.fifoMr->rkey;

    write(connfd, &local, sizeof(local));

    IbSend(buf, MSG_SIZE, mr, qp, &sendComm, 123);
    printf("after IbSend\n");
    uint64_t wr_ids[1] = {123};
    int mask[1] = {0};
    while(1) {
        IbTest(wr_ids, 1, mask, cq);
        if (mask[0]) {
            printf("Send Successful\n");
            break; // Exit loop after receiving the message
        }
    }
    strcpy(buf, "Bye!");
    IbSend(buf, MSG_SIZE, mr, qp, &sendComm, 456);
    while(1) {
        IbTest(wr_ids, 1, mask, cq);
        if (mask[0]) {
            printf("Send Successful\n");
            break; // Exit loop after receiving the message
        }
    }
    return 0;
}