#ifndef NIC_LOAD_H
#define NIC_LOAD_H

#include <stdatomic.h>
#include <stddef.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_NIC_COUNT 16

typedef struct {
    atomic_int tx;
    atomic_int rx;
} nic_load_t;

typedef struct {
    nic_load_t nics[MAX_NIC_COUNT];
    size_t nic_count;
} nic_load_table_t;

// Initialize the NIC load table with the given number of NICs
void nic_load_init(nic_load_table_t* table, size_t nic_count);

// Add tx load to a NIC
void add_nic_tx_load(nic_load_table_t* table, int nic_id, int tx_delta);
// Add rx load to a NIC
void add_nic_rx_load(nic_load_table_t* table, int nic_id, int rx_delta);
// Reduce tx load from a NIC
void reduce_nic_tx_load(nic_load_table_t* table, int nic_id, int tx_delta);
// Reduce rx load from a NIC
void reduce_nic_rx_load(nic_load_table_t* table, int nic_id, int rx_delta);
// Get the NIC id with the minimal tx load
int get_nic_with_min_tx_load(nic_load_table_t* table);
// Get the NIC id with the minimal rx load
int get_nic_with_min_rx_load(nic_load_table_t* table);
// Get the NIC id with the minimal total load (tx + rx)
int get_nic_with_min_load(nic_load_table_t* table);

#define NIC_LOAD_SHM_NAME "/nic_load_shm"

// Create and initialize shared memory for NIC load table. Returns pointer to mapped region, or NULL on error.
nic_load_table_t* nic_load_shm_create(size_t nic_count);

// Attach to an existing shared memory region for NIC load table. Returns pointer to mapped region, or NULL on error.
nic_load_table_t* nic_load_shm_attach();

// Detach from the shared memory region.
void nic_load_shm_detach(nic_load_table_t* table);

// Remove the shared memory object (should be called once, e.g., by the creator during cleanup).
void nic_load_shm_unlink();

#endif // NIC_LOAD_H 