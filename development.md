## FuseLink Development

### Overview

FuseLink data transmission consists of the following stages as a pipeline:
0. SET UP channels (channel/qp/nic bring up, net buffer, mr register)
1. Task splitting (done)
2. Task assignment to GPU, CPU (ongoing)
3. GPU fill network buffer (done)
4. CPU thread initiate RDMA transmission (ongoing)
5. Notify receiver GPUs and control threads (ongoing)
6. Free network resources for the task (ongoing)

### Task splitting

Split task as regular sized buffers.


### Task assignment

For each task, assign to the *idle NIC* or *direct NIC* for data transmission.

**NIC selection**

NIC selection is based on the load of NICs on both sender and receiver side.


### GPU fill network buffer

GPU thread copy source data to the network buffer (need to enable GPU p2p access).


### CPU thread initiate RDMA transmission

Receivers first notify senders about the NIC selected for data transmission.
Senders will transmit data with immediate numbers encoded for current NIC utilization.

### Notify receiver CPUs

receiver CPU threads poll for completion.

### receiver GPU threads copy

receiver GPU threads copy data to the recv buffer.
