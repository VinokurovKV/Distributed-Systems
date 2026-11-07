#ifndef MERGE_H
#define MERGE_H

#include <mpi.h>
#include <stdint.h>

enum { REQUEST_TAG = 100, DATA_TAG = 101, PROCESS_COUNT = 8 };

typedef struct {
    // считаем только отправки, без повторного учёта приёмов
    uint64_t requests;
    uint64_t blocks;
    uint64_t elements;
    // порядок запросов на корне
    int owners[PROCESS_COUNT - 1];
    int owner_count;
} MergeStats;

// у корня result[8*n] и два буфера по n, у каждого процесса свой local[n]
void merge_blocks(const int32_t *local, int n, int rank, int32_t *result,
                  int32_t *buffer_a, int32_t *buffer_b, MergeStats *stats);

#endif
