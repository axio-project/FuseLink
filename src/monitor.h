#ifndef MONITOR_H
#define MONITOR_H

#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <string>
#include <map>
#include <sys/mman.h>
#include <fcntl.h>
#include <semaphore.h>
#include <unistd.h>
// shm
#include <sys/shm.h>

#define MAX_NIC_NUM 8
#define NNIC_PER_GROUP 1
#define NGROUP (MAX_NIC_NUM / NNIC_PER_GROUP)
#define NCHANNELS_PER_NIC 2

#define SHM_NAME "/rdma_monitor_shm"
#define SEM_NAME "/rdma_monitor_sem"

#define MONITOR_TIMEOUT_US 160


inline int64_t get_timestamp() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

enum NicStatus {
    NIC_STATUS_IDLE = 0,
    NIC_STATUS_BUSY = 1,
    NIC_STATUS_BORROWED = 2,
};

// Shared memory structure for IPC
struct SharedMemory {
    NicStatus nic_tx_statuses[NGROUP][NNIC_PER_GROUP];
    NicStatus nic_rx_statuses[NGROUP][NNIC_PER_GROUP];
    int64_t nic_tx_last_active_ts[NGROUP][NNIC_PER_GROUP]; // timestamp
    int64_t nic_rx_last_active_ts[NGROUP][NNIC_PER_GROUP];

    uint64_t nic_tx_load[NGROUP][NNIC_PER_GROUP];
    uint64_t nic_rx_load[NGROUP][NNIC_PER_GROUP];

    std::atomic<bool> running;
};

class Monitor {
public:
    // Constructor and destructor
    Monitor();
    ~Monitor();

    // Initialize and detect available RDMA NICs
    bool initialize();
    
    // Stop monitoring
    void start();
    void stop();
    
    // Get number of detected RDMA NICs
    size_t getNicCount() const;
    
    // // Get specific NIC status
    // bool isChannelTxBusy(int group_id, int nic_id, int channel_id) const;
    // bool isChannelRxBusy(int group_id, int nic_id, int channel_id) const;
    
    // // Set specific NIC status
    // void setChannelTxStatus(int group_id, int nic_id, int channel_id, bool busy);
    // void setChannelRxStatus(int group_id, int nic_id, int channel_id, bool busy);

private:

    // Helper function to detect RDMA NICs
    bool detectRdmaNics();
    
    // Helper function to setup shared memory
    bool setupSharedMemory();

    // main thread
    void monitorThread();
    
    // Internal state
    std::atomic<bool> running_;
    SharedMemory* shared_memory_;
    int shm_fd_;
    sem_t* semaphore_;
    
    // Timeout for considering NIC idle (in milliseconds)
    static constexpr int IDLE_TIMEOUT_MS = 100;

    // monitor thread
    std::thread monitor_thread_;

    int ndev_;
};

// Client code to interact with monitor
class MonitorClient {
public:
    MonitorClient() {
        // Open shared memory
        shm_fd_ = shm_open(SHM_NAME, O_RDWR, 0666);
        if (shm_fd_ == -1) {
            throw std::runtime_error("Failed to open shared memory");
        }

        // Map shared memory
        shared_memory_ = static_cast<SharedMemory*>(
            mmap(nullptr, sizeof(SharedMemory), PROT_READ | PROT_WRITE, 
                 MAP_SHARED, shm_fd_, 0));
        if (shared_memory_ == MAP_FAILED) {
            close(shm_fd_);
            throw std::runtime_error("Failed to map shared memory");
        }

        // Open semaphore
        semaphore_ = sem_open(SEM_NAME, 0);
        if (semaphore_ == SEM_FAILED) {
            munmap(shared_memory_, sizeof(SharedMemory));
            close(shm_fd_);
            throw std::runtime_error("Failed to open semaphore");
        }
    }

    ~MonitorClient() {
        if (shared_memory_) {
            munmap(shared_memory_, sizeof(SharedMemory));
        }
        if (shm_fd_ != -1) {
            close(shm_fd_);
        }
        if (semaphore_) {
            sem_close(semaphore_);
        }
    }

    // void setChannelTxStatus(int group_id, int nic_id, int channel_id, bool busy) {
    //     if (group_id < 0 || group_id >= NGROUP || nic_id < 0 || nic_id >= NNIC_PER_GROUP || channel_id < 0 || channel_id >= NCHANNELS_PER_NIC) return;
    //     sem_wait(semaphore_);
    //     shared_memory_->channel_tx_statuses[group_id][nic_id][channel_id] = busy ? CHANNEL_STATUS_BUSY : CHANNEL_STATUS_IDLE;
    //     sem_post(semaphore_);
    // }

    // bool isChannelTxBusy(int group_id, int nic_id, int channel_id) const {
    //     if (group_id < 0 || group_id >= NGROUP || nic_id < 0 || nic_id >= NNIC_PER_GROUP || channel_id < 0 || channel_id >= NCHANNELS_PER_NIC) return false;
    //     sem_wait(semaphore_);
    //     bool busy = shared_memory_->channel_tx_statuses[group_id][nic_id][channel_id] == CHANNEL_STATUS_BUSY;
    //     sem_post(semaphore_);
    //     return busy;
    // }

    // void setChannelRxStatus(int group_id, int nic_id, int channel_id, bool busy) {
    //     if (group_id < 0 || group_id >= NGROUP || nic_id < 0 || nic_id >= NNIC_PER_GROUP || channel_id < 0 || channel_id >= NCHANNELS_PER_NIC) return;
    //     sem_wait(semaphore_);
    //     shared_memory_->channel_rx_statuses[group_id][nic_id][channel_id] = busy ? CHANNEL_STATUS_BUSY : CHANNEL_STATUS_IDLE;
    //     sem_post(semaphore_);
    // }

    // bool isChannelRxBusy(int group_id, int nic_id, int channel_id) const {
    //     if (group_id < 0 || group_id >= NGROUP || nic_id < 0 || nic_id >= NNIC_PER_GROUP || channel_id < 0 || channel_id >= NCHANNELS_PER_NIC) return false;
    //     sem_wait(semaphore_);
    //     bool busy = shared_memory_->channel_rx_statuses[group_id][nic_id][channel_id] == CHANNEL_STATUS_BUSY;
    //     sem_post(semaphore_);
    //     return busy;
    // }

    // bool isNicTxBusy(int group_id, int nic_id) const {
    //     for (int channel_id = 0; channel_id < NCHANNELS_PER_NIC; ++channel_id) {
    //         if (isChannelTxBusy(group_id, nic_id, channel_id)) return true;
    //     }
    //     return false;
    // }
    
    // bool isNicRxBusy(int group_id, int nic_id) const {
    //     for (int channel_id = 0; channel_id < NCHANNELS_PER_NIC; ++channel_id) {
    //         if (isChannelRxBusy(group_id, nic_id, channel_id)) return true;
    //     }
    //     return false;
    // }

    int getIdleNicTx(int group_id, int primary_nic_id) const { // try to borrow a NIC from the different group
        sem_wait(semaphore_);
        int idle_nic_id = -1;
        for (int i = 0; i < NGROUP; ++i) {
            if (i == group_id) continue;
            for (int j = 0; j < NNIC_PER_GROUP; ++j) {
                if (shared_memory_->nic_tx_statuses[i][j] == NIC_STATUS_IDLE) {
                    idle_nic_id = i * NNIC_PER_GROUP + j;
                    shared_memory_->nic_tx_statuses[i][j] = NIC_STATUS_BORROWED;
                    break;
                }
            }
        }
        sem_post(semaphore_);
        return idle_nic_id;
    }

    int getIdleNicRx(int group_id, int primary_nic_id, int mask=0xffffffff) const { // try to borrow a NIC from the different group
        sem_wait(semaphore_);
        int idle_nic_id = -1;
        for (int i = 0; i < NGROUP; ++i) {
            if (i == group_id) continue;
            for (int j = 0; j < NNIC_PER_GROUP; ++j) {
                if ((mask & (1 << (i * NNIC_PER_GROUP + j))) == 0) continue; // skip the NICs that are not in the mask
                if (shared_memory_->nic_rx_statuses[i][j] == NIC_STATUS_IDLE) {
                    idle_nic_id = i * NNIC_PER_GROUP + j;
                    shared_memory_->nic_rx_statuses[i][j] = NIC_STATUS_BORROWED;
                    break;
                }
            }
        }
        sem_post(semaphore_);
        return idle_nic_id;
    }

    void getNICStatus(int nNics, NicStatus* nic_status) {
      sem_wait(semaphore_);
      for (int i = 0; i < nNics; ++i) {
        int p = i / NNIC_PER_GROUP;
        int q = i % NNIC_PER_GROUP;
        nic_status[i] = shared_memory_->nic_tx_statuses[p][q];
      }
      sem_post(semaphore_);
    }

    void markNicTxAsActive(int group_id, int nic_id) {
        sem_wait(semaphore_);
        shared_memory_->nic_tx_last_active_ts[group_id][nic_id] = get_timestamp();
        // TODO: check if we need to set as borrowed
        shared_memory_->nic_tx_statuses[group_id][nic_id] = NIC_STATUS_BUSY;
        sem_post(semaphore_);
    }

    void markNicRxAsActive(int group_id, int nic_id) {
        sem_wait(semaphore_);
        shared_memory_->nic_rx_last_active_ts[group_id][nic_id] = get_timestamp();
        // TODO: check if we need to set as borrowed
        shared_memory_->nic_rx_statuses[group_id][nic_id] = NIC_STATUS_BUSY;
        sem_post(semaphore_);
    }

private:
    int shm_fd_;
    SharedMemory* shared_memory_;
    sem_t* semaphore_;
};

#endif // MONITOR_H
