#include "merge.h"
#include "mpi_helpers.h"

#include <stddef.h>

// текущее положение в одной распределённой последовательности
typedef struct {
    int owner;
    int end_owner;  // первый ранг за пределами последовательности
    int position;
    const int32_t *current;
    int32_t *buffer;
} Stream;

// загружаем блок, только если предыдущий уже использован
static void ensure_block(Stream *stream, int n, MergeStats *stats)
{
    if (stream->owner == stream->end_owner || stream->current != NULL) {
        return;
    }

    // владельцу достаточно пустого запроса, размер блока известен заранее
    MPI_CALL(MPI_Ssend(NULL, 0, MPI_BYTE, stream->owner, REQUEST_TAG,
                       MPI_COMM_WORLD));
    stats->requests++;

    MPI_Status status;
    MPI_CALL(MPI_Recv(stream->buffer, n, MPI_INT32_T, stream->owner, DATA_TAG,
                      MPI_COMM_WORLD, &status));
    // неполный ответ нельзя использовать как готовый блок
    int received = 0;
    MPI_CALL(MPI_Get_count(&status, MPI_INT32_T, &received));
    if (received != n || stats->owner_count >= PROCESS_COUNT - 1) {
        protocol_fail("Invalid block response");
    }
    stats->owners[stats->owner_count++] = stream->owner;
    stream->current = stream->buffer;
}

// сдвигаем позицию только в той последовательности, откуда взяли элемент
static void advance(Stream *stream, int n)
{
    stream->position++;
    if (stream->position == n) {
        stream->position = 0;
        // блок закончился, переходим к следующему владельцу
        stream->owner++;
        stream->current = NULL;  // следующий блок ещё не загружен
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
    const size_t length = (size_t)n * PROCESS_COUNT;

    for (size_t out = 0; out < length; out++) {
        // до сравнения должны быть доступны головы обеих непустых последовательностей
        ensure_block(&a, n, stats);
        ensure_block(&b, n, stats);

        Stream *selected;
        // при равенстве берём элемент первой последовательности
        if (b.owner == b.end_owner ||
            (a.owner != a.end_owner &&
             a.current[a.position] <= b.current[b.position])) {
            selected = &a;
        } else {
            selected = &b;
        }
        // записываем одно вхождение, поэтому повторы сохраняются
        result[out] = selected->current[selected->position];
        advance(selected, n);
    }
}
