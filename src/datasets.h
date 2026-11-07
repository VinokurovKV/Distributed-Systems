#ifndef DATASETS_H
#define DATASETS_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    CASE_DISJOINT,
    CASE_REVERSE,
    CASE_ALTERNATING,
    CASE_EQUAL,
    CASE_BOUNDARY,
    CASE_RANDOM,
    CASE_NEGATIVE,
    CASE_EXTREMES,
    CASE_COUNT
} Dataset;

const char *dataset_name(Dataset dataset);
int dataset_parse(const char *name, Dataset *dataset);
int32_t dataset_value(Dataset dataset, int n, int sequence, size_t position,
                      uint64_t seed);
void dataset_fill(int32_t *local, int n, int rank, Dataset dataset, uint64_t seed);
int dataset_local_valid(const int32_t *local, int n, int rank, Dataset dataset,
                        uint64_t seed);
int dataset_verify_result(const int32_t *result, int n, Dataset dataset,
                          uint64_t seed);

#endif
