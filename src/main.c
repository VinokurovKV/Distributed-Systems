#include "datasets.h"
#include "merge.h"
#include "mpi_helpers.h"

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    int n;
    Dataset dataset;
    uint64_t seed;
    int trace;
    int help;
} Options;

enum {
    DEFAULT_N = 16,     // размер блока при запуске без аргументов
    DEFAULT_SEED = 54,  // начальное значение генератора случайных данных
    MEASUREMENT_DONE_TAG = 200
};

#ifndef MERGE_SELF_TEST
// краткая справка по обычному запуску
static void usage(void)
{
    printf("запуск: merge [N [сценарий]] [--trace]\n"
           "сценарии: disjoint, reverse, alternating, equal, boundary,\n"
           "          random, negative, extremes\n"
           "по умолчанию: N=%d, alternating; нужно 8 процессов\n"
           "N от 1 до %" PRId32 "; --trace выводит обмены и модельное время\n",
           DEFAULT_N, INT32_MAX / 8);
}

// принимаем только целое неотрицательное число без знака и лишних символов
static int parse_number(const char *text, uint64_t *value)
{
    if (*text == '\0') {
        return 0;
    }
    for (const char *p = text; *p != '\0'; p++) {
        if (*p < '0' || *p > '9') {
            return 0;
        }
    }
    errno = 0;
    char *end = NULL;
    const unsigned long long parsed = strtoull(text, &end, 10);
    if (errno == ERANGE || *end != '\0' || parsed > UINT64_MAX) {
        return 0;
    }
    *value = (uint64_t)parsed;
    return 1;
}

// читаем размер блока, сценарий и необязательный вывод трассы
static int parse_options(int argc, char **argv, Options *options)
{
    *options = (Options){DEFAULT_N, CASE_ALTERNATING, DEFAULT_SEED, 0, 0};
    int positional = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0) {
            options->help = 1;
        } else if (strcmp(argv[i], "--trace") == 0) {
            options->trace = 1;
        } else if (positional == 0) {
            uint64_t n = 0;
            // ограничение сохраняет значения всех генераторов в диапазоне int32
            if (!parse_number(argv[i], &n) || n == 0 ||
                n > (uint64_t)(INT32_MAX / 8) || n > (uint64_t)INT_MAX) {
                return 0;
            }
            options->n = (int)n;
            positional++;
        } else if (positional == 1) {
            if (!dataset_parse(argv[i], &options->dataset)) {
                return 0;
            }
            positional++;
        } else {
            return 0;
        }
    }
    return 1;
}
#endif

// проверяем размер до умножения на число байт одного элемента
static int32_t *allocate_elements(size_t count)
{
    if (count > SIZE_MAX / sizeof(int32_t)) {
        protocol_fail("Array size overflows size_t");
    }
    int32_t *memory = malloc(count * sizeof(*memory));
    if (memory == NULL) {
        protocol_fail("Cannot allocate sequence storage");
    }
    return memory;
}

// каждый удалённый владелец должен встретиться в трассе ровно один раз
static int verify_order(const MergeStats *stats, Dataset dataset)
{
    if (stats->owner_count != PROCESS_COUNT - 1) {
        return 0;
    }
    unsigned seen = 0;  // по одному биту на владельца
    for (int i = 0; i < stats->owner_count; i++) {
        const int owner = stats->owners[i];
        if (owner < 1 || owner >= PROCESS_COUNT ||
            (seen & (1U << (unsigned)owner)) != 0) {
            return 0;
        }
        seen |= 1U << (unsigned)owner;
    }
    // для этих сценариев порядок запросов известен заранее
    const int forward[] = {4, 1, 2, 3, 5, 6, 7};
    const int reverse[] = {4, 5, 6, 7, 1, 2, 3};
    const int alternating[] = {4, 1, 5, 2, 6, 3, 7};
    const int *expected = NULL;
    if (dataset == CASE_DISJOINT || dataset == CASE_EQUAL) {
        expected = forward;
    } else if (dataset == CASE_REVERSE) {
        expected = reverse;
    } else if (dataset == CASE_ALTERNATING || dataset == CASE_NEGATIVE) {
        expected = alternating;
    }
    for (int i = 0; expected != NULL && i < stats->owner_count; i++) {
        if (stats->owners[i] != expected[i]) {
            return 0;
        }
    }
    return 1;
}

// выводим модельные времена обменов, не время реальной сети
static void print_trace(const MergeStats *stats, int n)
{
    uint64_t time = 0;
    const uint64_t bytes = 4 * (uint64_t)n;
    for (int i = 0; i < stats->owner_count; i++) {
        const int owner = stats->owners[i];
        printf("REQUEST 0 -> %d tag=%d bytes=0 model=[%" PRIu64 ",%" PRIu64 "]\n",
               owner, REQUEST_TAG, time, time + 100);
        time += 100;
        printf("DATA %d -> 0 tag=%d bytes=%" PRIu64 " model=[%" PRIu64
               ",%" PRIu64 "]\n", owner, DATA_TAG, bytes, time, time + 100 + bytes);
        time += 100 + bytes;
    }
}

// готовим один случай, измеряем слияние и проверяем результат
static int run_case(const Options *options, int rank, int quiet)
{
    const int n = options->n;
    int32_t *local = allocate_elements((size_t)n);
    int32_t *result = NULL;
    int32_t *buffers = NULL;
    if (rank == 0) {
        // итог 8n и два буфера по n выделяются только корню
        result = allocate_elements(8 * (size_t)n);
        buffers = allocate_elements(2 * (size_t)n);
    }
    dataset_fill(local, n, rank, options->dataset, options->seed);

    // до запуска убеждаемся, что все исходные блоки подготовлены правильно
    int valid = dataset_local_valid(local, n, rank, options->dataset, options->seed);
    int all_valid = 0;
    MPI_CALL(MPI_Allreduce(&valid, &all_valid, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD));
    if (!all_valid) {
        protocol_fail("Generated inputs are not globally sorted");
    }

    // измеряем только слияние
    MPI_CALL(MPI_Barrier(MPI_COMM_WORLD));
    const double start = MPI_Wtime();
    MergeStats stats;
    merge_blocks(local, n, rank, result, buffers,
                 rank == 0 ? buffers + n : NULL, &stats);
    const double elapsed = MPI_Wtime() - start;

    // проверка начинается после готовности результата и остановки таймера
    if (rank == 0) {
        for (int worker = 1; worker < PROCESS_COUNT; worker++) {
            MPI_CALL(MPI_Ssend(NULL, 0, MPI_BYTE, worker, MEASUREMENT_DONE_TAG,
                               MPI_COMM_WORLD));
        }
    } else {
        MPI_CALL(MPI_Recv(NULL, 0, MPI_BYTE, 0, MEASUREMENT_DONE_TAG,
                          MPI_COMM_WORLD, MPI_STATUS_IGNORE));
    }

    // итоговое реальное время берём по самому медленному процессу
    double maximum = 0;
    MPI_CALL(MPI_Reduce(&elapsed, &maximum, 1, MPI_DOUBLE, MPI_MAX, 0,
                        MPI_COMM_WORLD));
    // суммируем только отправки, чтобы не считать одно сообщение дважды
    const uint64_t sent[] = {stats.requests, stats.blocks, stats.elements};
    uint64_t totals[3] = {0};
    MPI_CALL(MPI_Reduce(sent, totals, 3, MPI_UINT64_T, MPI_SUM, 0, MPI_COMM_WORLD));

    // каждый владелец проверяет сохранность своего исходного блока
    valid = dataset_local_valid(local, n, rank, options->dataset, options->seed);
    if (rank == 0) {
        valid = valid && dataset_verify_result(result, n, options->dataset,
                                               options->seed);
        // сверяем фактические обмены с расчётом протокола
        valid = valid && totals[0] == 7 && totals[1] == 7 &&
                totals[2] == 7 * (uint64_t)n && verify_order(&stats, options->dataset);
    }
    MPI_CALL(MPI_Allreduce(&valid, &all_valid, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD));

    if (rank == 0) {
        if (!quiet || !all_valid) {
            const uint64_t messages = totals[0] + totals[1];
            const uint64_t bytes = 4 * totals[2];
            printf("case=%s n=%d seed=%" PRIu64 " verification=%s\n",
                   dataset_name(options->dataset), n, options->seed,
                   all_valid ? "PASS" : "FAIL");
            printf("requests=%" PRIu64 " blocks=%" PRIu64 " messages=%" PRIu64
                   " data_bytes=%" PRIu64 " model_time=%" PRIu64 "\n",
                   totals[0], totals[1], messages, bytes, 100 * messages + bytes);
            printf("wall_max_seconds=%.9f extra_elements_root=%" PRIu64 "\n",
                   maximum, 2 * (uint64_t)n);
            printf("harness_done_messages=7 (outside merge interval)\n");
        }
        if (options->trace) {
            print_trace(&stats, n);
        }
    }
    free(buffers);
    free(result);
    free(local);
    return all_valid;
}

#ifdef MERGE_SELF_TEST
// прогоняем все сценарии на малых размерах и дополнительные случайные случаи
static int self_test(const Options *options, int rank)
{
    // размеры для самопроверки
    const int sizes[] = {1, 2, 3, 7, 16, 31, 64};
    Options test = *options;
    int count = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        test.n = sizes[i];
        for (int dataset = 0; dataset < CASE_COUNT; dataset++) {
            test.dataset = (Dataset)dataset;
            if (!run_case(&test, rank, 1)) {
                return 0;
            }
            count++;
        }
    }
    // меняем seed при фиксированном размере, чтобы получить разные входы
    test.n = 17;
    test.dataset = CASE_RANDOM;
    for (uint64_t i = 0; i < 16; i++) {
        test.seed = options->seed + i;
        if (!run_case(&test, rank, 1)) {
            return 0;
        }
        count++;
    }
    if (rank == 0) {
        printf("self-test: PASS (%d distributed cases)\n", count);
    }
    return 1;
}
#endif

// проверяем окружение и выбираем обычный запуск или самопроверку
int main(int argc, char **argv)
{
    if (MPI_Init(&argc, &argv) != MPI_SUCCESS) {
        fprintf(stderr, "MPI_Init failed\n");
        return 1;
    }
    MPI_CALL(MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN));
    int rank = 0;
    int size = 0;
    MPI_CALL(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
    MPI_CALL(MPI_Comm_size(MPI_COMM_WORLD, &size));

#ifdef MERGE_SELF_TEST
    // make test собирает отдельную программу из тех же исходников
    const Options options = {DEFAULT_N, CASE_ALTERNATING, DEFAULT_SEED, 0, 0};
#else
    Options options;
    if (!parse_options(argc, argv, &options)) {
        if (rank == 0) {
            fprintf(stderr, "Invalid arguments. Use --help.\n");
        }
        MPI_CALL(MPI_Finalize());
        return 2;
    }
    if (options.help) {
        if (rank == 0) {
            usage();
        }
        MPI_CALL(MPI_Finalize());
        return 0;
    }
#endif
    // разбиение входа и протокол рассчитаны ровно на восемь процессов
    if (size != PROCESS_COUNT) {
        if (rank == 0) {
            fprintf(stderr, "Expected exactly 8 MPI processes, got %d\n", size);
        }
        MPI_CALL(MPI_Finalize());
        return 2;
    }
    char library[MPI_MAX_LIBRARY_VERSION_STRING];
    int length = 0;
    MPI_CALL(MPI_Get_library_version(library, &length));
    if (strstr(library, "Open MPI") == NULL) {
        protocol_fail("The loaded library is not Open MPI");
    }

#ifdef MERGE_SELF_TEST
    const int valid = self_test(&options, rank);
#else
    const int valid = run_case(&options, rank, 0);
#endif
    MPI_CALL(MPI_Finalize());
    return valid ? 0 : 1;
}
