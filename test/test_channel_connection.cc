#include "channel.h"
#include <infiniband/verbs.h>
#include <thread>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define DEV_ID 0

void server_thread() {
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        printf("Failed to get RDMA devices\n");
        exit(1);
    }
    SendChannel server_channel;
    printf("Server: Creating RDMA resources...\n");
    if (server_channel.create_rdmaResources(dev_list, DEV_ID) < 0) {
        printf("Server: RDMA resource creation failed\n");
        exit(1);
    }
    printf("Server: RDMA resources created.\n");
    // 监听端口 18515
    if (server_channel.Connect(18515) < 0) {
        printf("Server: Connect failed\n");
        exit(1);
    }
    printf("Server: RDMA connection established.\n");
    ibv_free_device_list(dev_list);
}

void client_thread() {
    // 等待server启动
    std::this_thread::sleep_for(std::chrono::seconds(1));
    struct ibv_device **dev_list = ibv_get_device_list(NULL);
    if (!dev_list) {
        printf("Failed to get RDMA devices\n");
        exit(1);
    }
    RecvChannel client_channel;
    if (client_channel.create_rdmaResources(dev_list, DEV_ID) < 0) {
        printf("Client: RDMA resource creation failed\n");
        exit(1);
    }
    printf("Client: RDMA resources created.\n");
    // 连接到本地18515端口
    if (client_channel.Connect("127.0.0.1", 18515) < 0) {
        printf("Client: Connect failed\n");
        exit(1);
    }
    printf("Client: RDMA connection established.\n");
    ibv_free_device_list(dev_list);
}

int main() {
    std::thread t_server(server_thread);
    std::thread t_client(client_thread);
    t_server.join();
    t_client.join();
    printf("Test passed.\n");
    return 0;
}