#include "monitor.h"
#include <chrono>
#include <iostream>
#include <infiniband/verbs.h>
#include <unistd.h>
#include <cstring>
#include <errno.h>

Monitor::Monitor() : running_(false), shared_memory_(nullptr), shm_fd_(-1), semaphore_(nullptr) {
}

Monitor::~Monitor() {
    stop();
    
    // Clean up shared memory
    if (shared_memory_) {
        munmap(shared_memory_, sizeof(SharedMemory));
        shared_memory_ = nullptr;
    }
    if (shm_fd_ != -1) {
        close(shm_fd_);
        shm_unlink(SHM_NAME);
    }
    if (semaphore_) {
        sem_close(semaphore_);
        sem_unlink(SEM_NAME);
    }
}

bool Monitor::initialize() {
    if (!detectRdmaNics()) {
        return false;
    }
    return setupSharedMemory();
}

bool Monitor::setupSharedMemory() {
    // Create shared memory object
    shm_fd_ = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd_ == -1) {
        std::cerr << "Failed to create shared memory: " << strerror(errno) << std::endl;
        return false;
    }

    // Set size of shared memory
    if (ftruncate(shm_fd_, sizeof(SharedMemory)) == -1) {
        std::cerr << "Failed to set shared memory size: " << strerror(errno) << std::endl;
        close(shm_fd_);
        shm_unlink(SHM_NAME);
        return false;
    }

    // Map shared memory
    shared_memory_ = static_cast<SharedMemory*>(mmap(nullptr, sizeof(SharedMemory),
        PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd_, 0));
    if (shared_memory_ == MAP_FAILED) {
        std::cerr << "Failed to map shared memory: " << strerror(errno) << std::endl;
        close(shm_fd_);
        shm_unlink(SHM_NAME);
        return false;
    }

    // Initialize shared memory
    memset(shared_memory_, 0, sizeof(SharedMemory));
    // for (int i = 0; i < MAX_NIC_NUM; ++i) {
    //     for (int j = 0; j < NNIC_PER_GROUP; ++j) {
    //         for (int k = 0; k < NCHANNELS_PER_NIC; ++k) {
    //             shared_memory_->channel_tx_statuses[i][j][k] = CHANNEL_STATUS_IDLE;
    //             shared_memory_->channel_rx_statuses[i][j][k] = CHANNEL_STATUS_IDLE;
    //         }
    //     }
    // }
    shared_memory_->running.store(true);

    // Create semaphore
    semaphore_ = sem_open(SEM_NAME, O_CREAT, 0666, 1);
    if (semaphore_ == SEM_FAILED) {
        std::cerr << "Failed to create semaphore: " << strerror(errno) << std::endl;
        munmap(shared_memory_, sizeof(SharedMemory));
        close(shm_fd_);
        shm_unlink(SHM_NAME);
        return false;
    }

    return true;
}

bool Monitor::detectRdmaNics() {
    int num_devices = 0;
    struct ibv_device** devices = ibv_get_device_list(&num_devices);
    
    if (!devices || num_devices <= 0) {
        std::cerr << "No RDMA devices found" << std::endl;
        return false;
    }

    ibv_free_device_list(devices);
    ndev_ = num_devices;
    return true;
}

void Monitor::start() {
    if (running_) return;
    
    running_ = true;
    // monitor_thread_ = std::thread(&Monitor::monitorThread, this);
}

void Monitor::stop() {
    if (shared_memory_) {
        shared_memory_->running.store(false);
    }
    // monitor_thread_.join();
}

size_t Monitor::getNicCount() const {
    // get nic count with ibv_get_device_list
    return ndev_;
}

// bool Monitor::isNicTxBusy(int nic_id) const {
//     if (nic_id < 0 || nic_id >= MAX_NIC_NUM) {
//         return false;
//     }
//     sem_wait(semaphore_);
//     bool busy = shared_memory_->nic_tx_statuses[nic_id] == NIC_STATUS_BUSY;
//     sem_post(semaphore_);
//     return busy;
// }

// bool Monitor::isNicRxBusy(int nic_id) const {
//     if (nic_id < 0 || nic_id >= MAX_NIC_NUM) {
//         return false;
//     }
//     sem_wait(semaphore_);
//     bool busy = shared_memory_->nic_rx_statuses[nic_id] == NIC_STATUS_BUSY;
//     sem_post(semaphore_);
//     return busy;
// }

// void Monitor::setNicTxStatus(int nic_id, bool busy) {
//     if (nic_id < 0 || nic_id >= MAX_NIC_NUM) {
//         return;
//     }
//     sem_wait(semaphore_);
//     shared_memory_->nic_tx_statuses[nic_id] = busy ? NIC_STATUS_BUSY : NIC_STATUS_IDLE;
//     sem_post(semaphore_);
// }

// void Monitor::setNicRxStatus(int nic_id, bool busy) {
//     if (nic_id < 0 || nic_id >= MAX_NIC_NUM) {
//         return;
//     }
//     sem_wait(semaphore_);
//     shared_memory_->nic_rx_statuses[nic_id] = busy ? NIC_STATUS_BUSY : NIC_STATUS_IDLE;
//     sem_post(semaphore_);
// }

void Monitor::monitorThread() {
    // while running and shared memory is running
    while (running_ && shared_memory_->running.load()) {
        // Check each NIC's status
        // execute once every MONITOR_TIMEOUT_US
        int64_t current_ts = get_timestamp();
        for (int i = 0; i < NGROUP; ++i) {
            // blocked when the shared memory is being updated by clients
            sem_wait(semaphore_);
            // std::cout << "monitor thread update nic status" << std::endl;
            // check status of each channel
            for (int j = 0; j < NNIC_PER_GROUP; ++j) {
                // check nic status with current status and last active timestamp
                if (shared_memory_->nic_tx_last_active_ts[i][j] + MONITOR_TIMEOUT_US > current_ts) {
                    // keep the same status
                } else {
                    // std::cout << "nic " << i << " " << j << " is idle" << std::endl;
                    shared_memory_->nic_tx_statuses[i][j] = NIC_STATUS_IDLE;
                }
            }
            // rx
            for (int j = 0; j < NNIC_PER_GROUP; ++j) {
                if (shared_memory_->nic_rx_last_active_ts[i][j] + MONITOR_TIMEOUT_US > current_ts) {
                    // keep the same status
                } else {
                    // std::cout << "nic " << i << " " << j << " is idle" << std::endl;
                    shared_memory_->nic_rx_statuses[i][j] = NIC_STATUS_IDLE;
                }
            }
            sem_post(semaphore_);
        }
        std::this_thread::sleep_for(std::chrono::microseconds(MONITOR_TIMEOUT_US));
    }
        
}

