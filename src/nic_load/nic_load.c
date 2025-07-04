#include "nic_load.h"
#include <limits.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

void nic_load_init(nic_load_table_t* table, size_t nic_count) {
    if (nic_count > MAX_NIC_COUNT) {
        nic_count = MAX_NIC_COUNT;
    }
    table->nic_count = nic_count;
    for (size_t i = 0; i < nic_count; ++i) {
        atomic_init(&table->nics[i].tx, 0);
        atomic_init(&table->nics[i].rx, 0);
    }
}

void add_nic_tx_load(nic_load_table_t* table, int nic_id, int tx_delta) {
    if (nic_id < 0 || (size_t)nic_id >= table->nic_count) return;
    atomic_fetch_add(&table->nics[nic_id].tx, tx_delta);
}

void add_nic_rx_load(nic_load_table_t* table, int nic_id, int rx_delta) {
    if (nic_id < 0 || (size_t)nic_id >= table->nic_count) return;
    atomic_fetch_add(&table->nics[nic_id].rx, rx_delta);
}

void reduce_nic_tx_load(nic_load_table_t* table, int nic_id, int tx_delta) {
    if (nic_id < 0 || (size_t)nic_id >= table->nic_count) return;
    atomic_fetch_sub(&table->nics[nic_id].tx, tx_delta);
}

void reduce_nic_rx_load(nic_load_table_t* table, int nic_id, int rx_delta) {
    if (nic_id < 0 || (size_t)nic_id >= table->nic_count) return;
    atomic_fetch_sub(&table->nics[nic_id].rx, rx_delta);
}

int get_nic_with_min_tx_load(nic_load_table_t* table) {
    int min_id = -1;
    int min_load = INT_MAX;
    for (size_t i = 0; i < table->nic_count; ++i) {
        int tx = atomic_load(&table->nics[i].tx);
        if (tx < min_load) {
            min_load = tx;
            min_id = (int)i;
        }
    }
    return min_id;
}

int get_nic_with_min_rx_load(nic_load_table_t* table) {
    int min_id = -1;
    int min_load = INT_MAX;
    for (size_t i = 0; i < table->nic_count; ++i) {
        int rx = atomic_load(&table->nics[i].rx);
        if (rx < min_load) {
            min_load = rx;
            min_id = (int)i;
        }
    }
    return min_id;
}

int get_nic_with_min_load(nic_load_table_t* table) {
    int min_id = -1;
    int min_load = INT_MAX;
    for (size_t i = 0; i < table->nic_count; ++i) {
        int tx = atomic_load(&table->nics[i].tx);
        int rx = atomic_load(&table->nics[i].rx);
        int total = tx + rx;
        if (total < min_load) {
            min_load = total;
            min_id = (int)i;
        }
    }
    return min_id;
}

nic_load_table_t* nic_load_shm_create(size_t nic_count) {
    int shm_fd = shm_open(NIC_LOAD_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open");
        return NULL;
    }
    if (ftruncate(shm_fd, sizeof(nic_load_table_t)) == -1) {
        perror("ftruncate");
        close(shm_fd);
        shm_unlink(NIC_LOAD_SHM_NAME);
        return NULL;
    }
    void* addr = mmap(NULL, sizeof(nic_load_table_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        shm_unlink(NIC_LOAD_SHM_NAME);
        return NULL;
    }
    close(shm_fd);
    nic_load_table_t* table = (nic_load_table_t*)addr;
    memset(table, 0, sizeof(nic_load_table_t));
    nic_load_init(table, nic_count);
    return table;
}

nic_load_table_t* nic_load_shm_attach() {
    int shm_fd = shm_open(NIC_LOAD_SHM_NAME, O_RDWR, 0666);
    if (shm_fd == -1) {
        perror("shm_open");
        return NULL;
    }
    void* addr = mmap(NULL, sizeof(nic_load_table_t), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (addr == MAP_FAILED) {
        perror("mmap");
        close(shm_fd);
        return NULL;
    }
    close(shm_fd);
    return (nic_load_table_t*)addr;
}

void nic_load_shm_detach(nic_load_table_t* table) {
    if (table) {
        munmap(table, sizeof(nic_load_table_t));
    }
}

void nic_load_shm_unlink() {
    shm_unlink(NIC_LOAD_SHM_NAME);
} 