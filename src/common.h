#ifndef FUSELINK_COMMON_H
#define FUSELINK_COMMON_H

#define FUSELINK_CHUNK_SZ (1 << 21) // 2MB
#define CEIL_DIV(a, b) (((a) + (b) - 1) / (b))
#define FLOOR_DIV(a, b) ((a) / (b))
#define RING_SIZE 16

#endif