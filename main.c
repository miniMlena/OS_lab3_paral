#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct {
    int *array;
    size_t lo;
    size_t n;
    size_t r;
} SortContext;

typedef struct {
    int *array;
    size_t lo;
    size_t r;
    size_t first;
    size_t step;
    size_t count;
} WorkerArgs;

static size_t max_threads = 1;
static int verbose = 0;

static double now_seconds(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        perror("clock_gettime");
        exit(EXIT_FAILURE);
    }

    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void swap_if_needed(int *a, int *b)
{
    if (*a > *b) {
        int tmp = *a;
        *a = *b;
        *b = tmp;
    }
}

static void *compare_worker(void *arg)
{
    WorkerArgs *w = (WorkerArgs *)arg;

    for (size_t k = w->first; k < w->first + w->count; ++k) {
        size_t i = w->lo + w->r + k * w->step;
        swap_if_needed(&w->array[i], &w->array[i + w->r]);
    }

    return NULL;
}

static void parallel_compare(int *a, size_t lo, size_t n, size_t r)
{
    size_t step = 2 * r;
    size_t count = 0;

    for (size_t i = lo + r; i + r < lo + n; i += step) {
        ++count;
    }

    if (count == 0) {
        return;
    }

    size_t workers = max_threads > 1 ? max_threads - 1 : 0;
    if (workers > count) {
        workers = count;
    }

    if (workers == 0) {
        for (size_t i = lo + r; i + r < lo + n; i += step) {
            swap_if_needed(&a[i], &a[i + r]);
        }
        return;
    }

    pthread_t *threads = calloc(workers, sizeof(*threads));
    WorkerArgs *args = calloc(workers, sizeof(*args));
    int *created = calloc(workers, sizeof(*created));

    if (!threads || !args || !created) {
        free(threads);
        free(args);
        free(created);

        for (size_t i = lo + r; i + r < lo + n; i += step) {
            swap_if_needed(&a[i], &a[i + r]);
        }
        return;
    }

    size_t participants = workers + 1;
    size_t base = count / participants;
    size_t remainder = count % participants;
    size_t first = 0;

    for (size_t j = 0; j < workers; ++j) {
        size_t block = base + (j < remainder ? 1 : 0);

        args[j] = (WorkerArgs) {
            .array = a,
            .lo = lo,
            .r = r,
            .first = first,
            .step = step,
            .count = block
        };

        int rc = pthread_create(
            &threads[j], NULL, compare_worker, &args[j]);

        if (rc == 0) {
            created[j] = 1;
        } else {
            if (verbose) {
                fprintf(stderr, "pthread_create: %s\n", strerror(rc));
            }
            compare_worker(&args[j]);
        }

        first += block;
    }

    WorkerArgs main_args = {
        .array = a,
        .lo = lo,
        .r = r,
        .first = first,
        .step = step,
        .count = count - first
    };
    compare_worker(&main_args);

    /* Обязательно дожидаемся завершения всех созданных потоков. */
    for (size_t j = 0; j < workers; ++j) {
        if (created[j]) {
            int rc = pthread_join(threads[j], NULL);
            if (rc != 0) {
                fprintf(stderr, "pthread_join: %s\n", strerror(rc));
                abort();
            }
        }
    }

    free(created);
    free(args);
    free(threads);
}

static void odd_even_merge(int *a, size_t lo, size_t n, size_t r)
{
    size_t step = 2 * r;

    if (step < n) {
        odd_even_merge(a, lo, n, step);
        odd_even_merge(a, lo + r, n - r, step);

        parallel_compare(a, lo, n, r);
    } else {
        swap_if_needed(&a[lo], &a[lo + r]);
    }
}

static void batcher_sort(int *a, size_t lo, size_t n)
{
    if (n <= 1) {
        return;
    }

    size_t half = n / 2;

    batcher_sort(a, lo, half);
    batcher_sort(a, lo + half, half);
    odd_even_merge(a, lo, n, 1);
}

static int is_sorted(const int *a, size_t n)
{
    for (size_t i = 1; i < n; ++i) {
        if (a[i - 1] > a[i]) {
            return 0;
        }
    }
    return 1;
}

static void print_array(const int *a, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        printf("%d%s", a[i], i + 1 == n ? "\n" : " ");
    }
}

static int parse_size(const char *s, size_t *out)
{
    char *end = NULL;
    errno = 0;

    unsigned long long value = strtoull(s, &end, 10);

    if (errno != 0 || end == s || *end != '\0' ||
        value == 0 || value > SIZE_MAX) {
        return 0;
    }

    *out = (size_t)value;
    return 1;
}


//        "Использование: %s -n SIZE -t THREADS [-s SEED] [-v] [-p]\n"
//        "  -n SIZE     количество элементов массива\n"
//        "  -t THREADS  максимум потоков процесса, включая главный (>= 1)\n"
//        "  -s SEED     начальное значение генератора случайных чисел\n"
//      "  -v          вывести исходный и отсортированный массив\n"
//      "  -p          использовать уже заданный seed по умолчанию (1)\n",

int main(int argc, char **argv)
{
    size_t n = 0;
    size_t seed = 1;
    int seed_given = 0;
    int print_result = 0;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            if (!parse_size(argv[++i], &n)) {
                fprintf(stderr, "Некорректный размер массива.\n");
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            if (!parse_size(argv[++i], &max_threads)) {
                fprintf(stderr, "Некорректное количество потоков.\n");
                return EXIT_FAILURE;
            }
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            if (!parse_size(argv[++i], &seed)) {
                fprintf(stderr, "Некорректный seed.\n");
                return EXIT_FAILURE;
            }
            seed_given = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            print_result = 1;
            verbose = 1;
        } else if (strcmp(argv[i], "-p") == 0) {
            seed = 1;
            seed_given = 1;
        } else {
            return EXIT_FAILURE;
        }
    }

    if (n == 0 || max_threads == 0) {
        return EXIT_FAILURE;
    }

    size_t padded = 1;
    while (padded < n) {
        if (padded > SIZE_MAX / 2) {
            fprintf(stderr, "Размер массива слишком велик.\n");
            return EXIT_FAILURE;
        }
        padded *= 2;
    }

    if (padded > SIZE_MAX / sizeof(int)) {
        fprintf(stderr, "Размер массива слишком велик.\n");
        return EXIT_FAILURE;
    }

    int *a = malloc(padded * sizeof(*a));
    if (a == NULL) {
        perror("malloc");
        return EXIT_FAILURE;
    }

    if (!seed_given) {
        seed = (size_t)time(NULL);
    }
    srand((unsigned int)seed);

    for (size_t i = 0; i < n; ++i) {
        a[i] = rand() % 2000001 - 1000000;
    }

    for (size_t i = n; i < padded; ++i) {
        a[i] = INT_MAX;
    }

    if (print_result) {
        printf("Исходный массив (%zu элементов):\n", n);
        print_array(a, n);
    }

    double start = now_seconds();
    batcher_sort(a, 0, padded);
    double elapsed = now_seconds() - start;

    int ok = is_sorted(a, n);

    printf("N=%zu padded=%zu threads_limit=%zu seed=%zu\n",
           n, padded, max_threads, seed);
    printf("time_seconds=%.9f\n", elapsed);
    printf("sorted=%s\n", ok ? "yes" : "NO");

    if (print_result) {
        printf("Отсортированный массив:\n");
        print_array(a, n);
    }

    free(a);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;

}
