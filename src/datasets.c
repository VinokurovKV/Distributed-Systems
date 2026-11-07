#include "datasets.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// название сценария для запуска и вывода результатов
const char *dataset_name(Dataset dataset)
{
    switch (dataset) {
    case CASE_DISJOINT: return "disjoint";
    case CASE_REVERSE: return "reverse";
    case CASE_ALTERNATING: return "alternating";
    case CASE_EQUAL: return "equal";
    case CASE_BOUNDARY: return "boundary";
    case CASE_RANDOM: return "random";
    case CASE_NEGATIVE: return "negative";
    case CASE_EXTREMES: return "extremes";
    case CASE_COUNT: break;
    }
    return "unknown";
}

// переводим название из аргументов в код сценария
int dataset_parse(const char *name, Dataset *dataset)
{
    for (int candidate = 0; candidate < CASE_COUNT; candidate++) {
        if (strcmp(name, dataset_name((Dataset)candidate)) == 0) {
            *dataset = (Dataset)candidate;
            return 1;
        }
    }
    return 0;
}

// одинаковый ключ даёт одинаковое псевдослучайное значение на всех процессах
static uint64_t mix(uint64_t value)
{
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

// вычисляем элемент по глобальной позиции без хранения всей последовательности
int32_t dataset_value(Dataset dataset, int n, int sequence, size_t position,
                      uint64_t seed)
{
    const int64_t g = (int64_t)position;
    const int64_t length = 4 * (int64_t)n;
    int64_t value = 0;
    switch (dataset) {
    case CASE_DISJOINT:
        value = g + (sequence == 0 ? 0 : length);
        break;
    case CASE_REVERSE:
        value = g + (sequence == 0 ? length : 0);
        break;
    case CASE_ALTERNATING:
        // чётные значения у первой последовательности, нечётные у второй
        value = 2 * g + sequence;
        break;
    case CASE_EQUAL:
        value = 7;
        break;
    case CASE_BOUNDARY:
        // при n >= 2 последнее значение блока равно первому в следующем
        value = (g + 1) / n;
        break;
    case CASE_RANDOM: {
        // шаг 4 сохраняет порядок при случайном смещении от 0 до 3
        // деление на 3 создаёт повторы без дополнительного массива
        const uint64_t bucket = (uint64_t)position / 3;
        const uint64_t key = seed ^ (bucket * UINT64_C(0x9e3779b97f4a7c15)) ^
                             ((uint64_t)sequence * UINT64_C(0xd1b54a32d192ed03));
        value = (int64_t)(4 * bucket) + (int64_t)(mix(key) % 4) - 2 * (int64_t)n;
        break;
    }
    case CASE_NEGATIVE:
        value = 2 * g + sequence - 8 * (int64_t)n;
        break;
    case CASE_EXTREMES:
        // проверяем сравнения крайних значений без вычитания и переполнения
        if (sequence == 0) {
            value = g < 2 * (int64_t)n ? INT32_MIN : INT32_MAX;
        } else {
            value = g < n ? INT32_MIN : (g < 3 * (int64_t)n ? 0 : INT32_MAX);
        }
        break;
    case CASE_COUNT:
        abort();
    }
    return (int32_t)value;
}

// каждый процесс создаёт только свой блок из n элементов
void dataset_fill(int32_t *local, int n, int rank, Dataset dataset, uint64_t seed)
{
    // rank / 4 задаёт последовательность, rank % 4 задаёт номер блока в ней
    const size_t start = (size_t)(rank % 4) * (size_t)n;
    for (int i = 0; i < n; i++) {
        local[i] = dataset_value(dataset, n, rank / 4, start + (size_t)i, seed);
    }
}

// проверяем исходные значения и порядок внутри блока и на его левой границе
int dataset_local_valid(const int32_t *local, int n, int rank, Dataset dataset,
                        uint64_t seed)
{
    const size_t start = (size_t)(rank % 4) * (size_t)n;
    for (int i = 0; i < n; i++) {
        const size_t g = start + (size_t)i;
        // повторная генерация позволяет заметить изменение входа без его копии
        if (local[i] != dataset_value(dataset, n, rank / 4, g, seed)) {
            return 0;
        }
        if (i > 0 && local[i - 1] > local[i]) {
            return 0;
        }
        if (i == 0 && start > 0 &&
            dataset_value(dataset, n, rank / 4, start - 1, seed) > local[i]) {
            return 0;
        }
    }
    return 1;
}

// проверяем все элементы результата и кратности независимо от алгоритма слияния
int dataset_verify_result(const int32_t *result, int n, Dataset dataset,
                          uint64_t seed)
{
    const size_t length = 4 * (size_t)n;
    for (size_t i = 1; i < 2 * length; i++) {
        if (result[i - 1] > result[i]) {
            fprintf(stderr, "Result is not sorted at position %zu\n", i);
            return 0;
        }
    }

    // проверяем позицию каждого вхождения без эталонного массива
    for (int sequence = 0; sequence < 2; sequence++) {
        for (size_t g = 0; g < length; g++) {
            const int32_t value = dataset_value(dataset, n, sequence, g, seed);
            size_t low = 0;
            size_t high = length;
            // ищем число элементов другой последовательности, которые идут раньше
            while (low < high) {
                const size_t middle = low + (high - low) / 2;
                const int32_t other = dataset_value(dataset, n, 1 - sequence,
                                                    middle, seed);
                // равные элементы из первой последовательности идут раньше
                if (other < value || (sequence == 1 && other == value)) {
                    low = middle + 1;
                } else {
                    high = middle;
                }
            }
            // g своих предшественников и low элементов другой последовательности
            const size_t destination = g + low;
            if (result[destination] != value) {
                fprintf(stderr, "Mismatch at position %zu: expected %" PRId32
                        ", got %" PRId32 "\n", destination, value,
                        result[destination]);
                return 0;
            }
        }
    }
    return 1;
}
