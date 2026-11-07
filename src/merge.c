#include "merge.h"
#include "mpi_helpers.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

// текущее положение в одной распределённой последовательности
typedef struct {
    int owner;
    int end_owner;  // первый ранг за пределами последовательности
    int position;
    const int32_t *current;
    int32_t *buffer;
} Stream;

// запрашиваем блок в буфер или сразу в его окончательное место
static void request_block(int owner, int n, int32_t *destination, MergeStats *stats)
{
    if (stats->owner_count >= PROCESS_COUNT - 1) {
        protocol_fail("Число запросов превысило семь");
    }

    // владельцу достаточно пустого запроса, размер блока известен заранее
    MPI_CALL(MPI_Ssend(NULL, 0, MPI_BYTE, owner, REQUEST_TAG,
                       MPI_COMM_WORLD));
    stats->requests++;

    MPI_Status status;
    MPI_CALL(MPI_Recv(destination, n, MPI_INT32_T, owner, DATA_TAG,
                      MPI_COMM_WORLD, &status));
    // неполный ответ нельзя использовать как готовый блок
    int received = 0;
    MPI_CALL(MPI_Get_count(&status, MPI_INT32_T, &received));
    if (received != n) {
        fprintf(stderr, "rank 0: DATA от %d: ожидалось %d элементов, получено %d\n",
                owner, n, received);
        protocol_fail("Неполный ответ на запрос блока");
    }
    stats->owners[stats->owner_count++] = owner;
}

// вызывается один раз при исчерпании блока
static void next_block(Stream *stream, int n, MergeStats *stats)
{
    stream->position = 0;
    stream->owner++;
    stream->current = NULL;
    if (stream->owner < stream->end_owner) {
        request_block(stream->owner, n, stream->buffer, stats);
        stream->current = stream->buffer;
    }
}

// владельцы отвечают на запросы, корень выполняет само слияние
void merge_blocks(const int32_t *local, int n, int rank, int32_t *result,
                  int32_t *buffer_a, int32_t *buffer_b, MergeStats *stats)
{
    *stats = (MergeStats){0};

    if (rank != 0) {
        // каждый удалённый блок понадобится ровно один раз
        MPI_Status status;
        MPI_CALL(MPI_Recv(NULL, 0, MPI_BYTE, 0, REQUEST_TAG, MPI_COMM_WORLD,
                          &status));
        int received = 0;
        MPI_CALL(MPI_Get_count(&status, MPI_BYTE, &received));
        if (received != 0) {
            protocol_fail("Expected an empty block request");
        }
        // отправляем исходный блок напрямую, не меняя его содержимое
        MPI_CALL(MPI_Ssend(local, n, MPI_INT32_T, 0, DATA_TAG, MPI_COMM_WORLD));
        stats->blocks = 1;
        stats->elements = (uint64_t)n;
        return;
    }

    // свой блок уже доступен, первый блок второй последовательности запросим
    Stream a = {0, 4, 0, local, buffer_a};
    Stream b = {4, 8, 0, NULL, buffer_b};
    size_t out = 0;
    request_block(b.owner, n, b.buffer, stats);
    b.current = b.buffer;

    while (a.owner < a.end_owner && b.owner < b.end_owner) {
        // внутри блока только сравнения и запись, без вызовов загрузки
        while (a.position < n && b.position < n) {
            // при равенстве берём элемент первой последовательности
            if (a.current[a.position] <= b.current[b.position]) {
                result[out++] = a.current[a.position++];
            } else {
                result[out++] = b.current[b.position++];
            }
        }
        if (a.position == n) {
            next_block(&a, n, stats);
        } else {
            next_block(&b, n, stats);
        }
    }

    // оставшаяся последовательность уже упорядочена
    Stream *tail = a.owner < a.end_owner ? &a : &b;
    const size_t remaining = (size_t)(n - tail->position);
    memcpy(result + out, tail->current + tail->position,
           remaining * sizeof(*result));
    out += remaining;

    // следующие блоки принимаем сразу в итоговый массив
    for (int owner = tail->owner + 1; owner < tail->end_owner; owner++) {
        request_block(owner, n, result + out, stats);
        out += (size_t)n;
    }
    if (out != (size_t)n * PROCESS_COUNT) {
        protocol_fail("Неверное число записанных элементов результата");
    }
}
