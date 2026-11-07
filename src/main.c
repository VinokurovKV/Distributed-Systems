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
    WARMUP_RUNS = 1,    // прогревы перед измерениями
    MEASURED_RUNS = 7,  // число повторов для медианы и разброса
    MEASUREMENT_DONE_TAG = 200
};

_Static_assert(WARMUP_RUNS >= 0 && MEASURED_RUNS > 0, "Invalid repeat counts");

#ifndef MERGE_SELF_TEST
// сохраняем сведения об окружении рядом с результатами измерений
static void print_environment(int rank, const char *library)
{
    char host[MPI_MAX_PROCESSOR_NAME] = {0};
    char hosts[PROCESS_COUNT][MPI_MAX_PROCESSOR_NAME];
    int length = 0;
    MPI_CALL(MPI_Get_processor_name(host, &length));
    MPI_CALL(MPI_Gather(host, MPI_MAX_PROCESSOR_NAME, MPI_CHAR, hosts,
                        MPI_MAX_PROCESSOR_NAME, MPI_CHAR, 0, MPI_COMM_WORLD));
    if (rank == 0) {
        printf("MPI library: %s\n", library);
        printf("timer_resolution_seconds=%.9g\n", MPI_Wtick());
        for (int worker = 0; worker < PROCESS_COUNT; worker++) {
            printf("rank=%d host=%s\n", worker, hosts[worker]);
        }
    }
}

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
        fprintf(stderr, "rank 0: трасса: ожидалось 7 владельцев, получено %d\n",
                stats->owner_count);
        return 0;
    }
    unsigned seen = 0;  // по одному биту на владельца
    for (int i = 0; i < stats->owner_count; i++) {
        const int owner = stats->owners[i];
        if (owner < 1 || owner >= PROCESS_COUNT) {
            fprintf(stderr, "rank 0: трасса[%d]: недопустимый владелец %d\n",
                    i, owner);
            return 0;
        }
        if ((seen & (1U << (unsigned)owner)) != 0) {
            fprintf(stderr, "rank 0: трасса[%d]: повторный запрос владельцу %d\n",
                    i, owner);
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
            fprintf(stderr, "rank 0: трасса[%d]: ожидался владелец %d, получен %d\n",
                    i, expected[i], stats->owners[i]);
            return 0;
        }
    }
    return 1;
}

// отдельно проверяем каждый счётчик, чтобы указать причину расхождения
static int verify_totals(const uint64_t totals[3], int n)
{
    const uint64_t expected[] = {7, 7, 7 * (uint64_t)n};
    const char *names[] = {"запросы", "блоки", "элементы"};
    int valid = 1;
    for (int i = 0; i < 3; i++) {
        if (totals[i] != expected[i]) {
            fprintf(stderr, "rank 0: счётчик %s: ожидалось %" PRIu64
                    ", получено %" PRIu64 "\n", names[i], expected[i], totals[i]);
            valid = 0;
        }
    }
    return valid;
}

// не даём ранним владельцам начать проверку до готовности результата
static void finish_measurement(int rank)
{
    if (rank == 0) {
        for (int worker = 1; worker < PROCESS_COUNT; worker++) {
            MPI_CALL(MPI_Ssend(NULL, 0, MPI_BYTE, worker, MEASUREMENT_DONE_TAG,
                               MPI_COMM_WORLD));
        }
    } else {
        MPI_CALL(MPI_Recv(NULL, 0, MPI_BYTE, 0, MEASUREMENT_DONE_TAG,
                          MPI_COMM_WORLD, MPI_STATUS_IGNORE));
    }
}

// сортируем только небольшую таблицу длительностей
static void print_timings(double *samples, int count)
{
    for (int i = 1; i < count; i++) {
        const double value = samples[i];
        int j = i;
        while (j > 0 && samples[j - 1] > value) {
            samples[j] = samples[j - 1];
            j--;
        }
        samples[j] = value;
    }
    const double middle = samples[(count - 1) / 2];
    const double median = middle + (samples[count / 2] - middle) / 2;
    printf("wall_min_seconds=%.9f wall_median_seconds=%.9f wall_max_seconds=%.9f\n",
           samples[0], median, samples[count - 1]);
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
static int run_case(const Options *options, int rank, int test_mode)
{
    const int n = options->n;
    int32_t *local = allocate_elements((size_t)n);
    int32_t *result = NULL;
    int32_t *buffers = NULL;
    if (rank == 0) {
        // итог 8n и два буфера по n выделяются только корню
        result = allocate_elements(8 * (size_t)n);
        buffers = allocate_elements(2 * (size_t)n);
        // первое обращение к страницам памяти не должно попасть в измерение
        memset(result, 0, 8 * (size_t)n * sizeof(*result));
        memset(buffers, 0, 2 * (size_t)n * sizeof(*buffers));
    }
    dataset_fill(local, n, rank, options->dataset, options->seed);

    // до запуска убеждаемся, что все исходные блоки подготовлены правильно
    int valid = dataset_local_valid(local, n, rank, options->dataset, options->seed);
    int all_valid = 0;
    MPI_CALL(MPI_Allreduce(&valid, &all_valid, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD));
    if (!all_valid) {
        protocol_fail("Generated inputs are not globally sorted");
    }

    const int warmups = test_mode ? 0 : WARMUP_RUNS;
    const int repetitions = test_mode ? 1 : MEASURED_RUNS;
    double samples[MEASURED_RUNS];
    int sample_count = 0;
    MergeStats stats;
    uint64_t totals[3] = {0};
    if (rank == 0 && !test_mode) {
        printf("case=%s n=%d seed=%" PRIu64 " warmups=%d repetitions=%d\n",
               dataset_name(options->dataset), n, options->seed, warmups, repetitions);
    }
    for (int run = 0; run < warmups + repetitions; run++) {
        // измеряем только слияние, все массивы уже выделены и прогреты
        MPI_CALL(MPI_Barrier(MPI_COMM_WORLD));
        const double start = MPI_Wtime();
        merge_blocks(local, n, rank, result, buffers,
                     rank == 0 ? buffers + n : NULL, &stats);
        const double elapsed = MPI_Wtime() - start;
        finish_measurement(rank);

        // берём максимум по процессам отдельно для каждого повтора
        double maximum = 0;
        MPI_CALL(MPI_Reduce(&elapsed, &maximum, 1, MPI_DOUBLE, MPI_MAX, 0,
                            MPI_COMM_WORLD));
        const uint64_t sent[] = {stats.requests, stats.blocks, stats.elements};
        MPI_CALL(MPI_Reduce(sent, totals, 3, MPI_UINT64_T, MPI_SUM, 0, MPI_COMM_WORLD));

        valid = dataset_local_valid(local, n, rank, options->dataset, options->seed);
        if (!valid) {
            fprintf(stderr, "rank %d: сохранность входа нарушена после запуска %d\n",
                    rank, run + 1);
        }
        if (rank == 0) {
            // выполняем все проверки, даже если одна из них уже нашла ошибку
            const int result_valid = dataset_verify_result(result, n, options->dataset,
                                                            options->seed);
            const int counts_valid = verify_totals(totals, n);
            const int order_valid = verify_order(&stats, options->dataset);
            valid = valid && result_valid && counts_valid && order_valid;
        }
        MPI_CALL(MPI_Allreduce(&valid, &all_valid, 1, MPI_INT, MPI_MIN, MPI_COMM_WORLD));
        if (!all_valid) {
            if (rank == 0) {
                fprintf(stderr, "case=%s n=%d run=%d verification=FAIL\n",
                        dataset_name(options->dataset), n, run + 1);
            }
            break;
        }
        if (rank == 0 && run >= warmups) {
            samples[sample_count++] = maximum;
            if (!test_mode) {
                printf("sample=%d wall_max_seconds=%.9f verification=PASS\n",
                       sample_count, maximum);
            }
        }
    }

    if (rank == 0 && all_valid) {
        if (!test_mode) {
            const uint64_t messages = totals[0] + totals[1];
            const uint64_t bytes = 4 * totals[2];
            printf("verification=PASS\n");
            printf("statistics_scope=per_merge requests=%" PRIu64
                   " blocks=%" PRIu64 " messages=%" PRIu64
                   " data_bytes=%" PRIu64 " model_time=%" PRIu64 "\n",
                   totals[0], totals[1], messages, bytes, 100 * messages + bytes);
            printf("extra_elements_root=%" PRIu64 "\n", 2 * (uint64_t)n);
            printf("harness_done_messages=7 (per run, outside merge interval)\n");
            print_timings(samples, sample_count);
        }
        if (options->trace) {
            printf("trace_scope=last_merge\n");
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
    print_environment(rank, library);
    const int valid = run_case(&options, rank, 0);
#endif
    MPI_CALL(MPI_Finalize());
    return valid ? 0 : 1;
}
