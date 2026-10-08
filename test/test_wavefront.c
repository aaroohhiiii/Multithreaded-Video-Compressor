#define _POSIX_C_SOURCE 200809L

#include "wavefront.h"

#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

enum {
    TEST_ROWS = 6,
    TEST_COLUMNS = 12,
    TEST_RUNS = 25
};

typedef struct WorkerArgs {
    WavefrontSync *sync;
    int row;
    uint32_t random_state;
    int error;
} WorkerArgs;

typedef struct BlockingProbe {
    WavefrontSync *sync;
    atomic_int returned;
} BlockingProbe;

static uint32_t next_random(uint32_t *state)
{
    *state = *state * 1664525U + 1013904223U;
    return *state;
}

static void random_pause(uint32_t *state)
{
    struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = (long)(next_random(state) % 2000000U)
    };

    nanosleep(&delay, NULL);
}

static void *fake_row_worker(void *argument)
{
    WorkerArgs *args = argument;
    int column;

    for (column = 0; column < TEST_COLUMNS; ++column) {
        int result;

        random_pause(&args->random_state);
        result = wavefront_wait_for_dependency(args->sync, args->row, column);
        if (result != 0) {
            args->error = result;
            return NULL;
        }

        if (args->row > 0) {
            pthread_mutex_lock(&args->sync->mutex);
            if (args->sync->row_progress[args->row - 1] <= column) {
                args->error = 1;
            }
            pthread_mutex_unlock(&args->sync->mutex);
            if (args->error != 0) {
                return NULL;
            }
        }

        random_pause(&args->random_state);
        result = wavefront_mark_done(args->sync, args->row, column);
        if (result != 0) {
            args->error = result;
            return NULL;
        }
    }
    return NULL;
}

static void *dependency_probe(void *argument)
{
    BlockingProbe *probe = argument;

    assert(wavefront_wait_for_dependency(probe->sync, 1, 0) == 0);
    atomic_store(&probe->returned, 1);
    return NULL;
}

int main(void)
{
    int run;

    /* Convert a deadlock regression into a failed test instead of a hang. */
    alarm(15U);

    {
        WavefrontSync sync;
        BlockingProbe probe;
        pthread_t thread;
        struct timespec observation_delay = {.tv_sec = 0,
                                             .tv_nsec = 20000000L};

        assert(wavefront_init(&sync) == 0);
        probe.sync = &sync;
        atomic_init(&probe.returned, 0);
        assert(pthread_create(&thread, NULL, dependency_probe, &probe) == 0);
        nanosleep(&observation_delay, NULL);
        assert(atomic_load(&probe.returned) == 0);
        assert(wavefront_mark_done(&sync, 0, 0) == 0);
        assert(pthread_join(thread, NULL) == 0);
        assert(atomic_load(&probe.returned) == 1);
        assert(wavefront_destroy(&sync) == 0);
    }

    for (run = 0; run < TEST_RUNS; ++run) {
        WavefrontSync sync;
        pthread_t threads[TEST_ROWS];
        WorkerArgs arguments[TEST_ROWS];
        int creation_order[TEST_ROWS];
        uint32_t shuffle_state = (uint32_t)run + 1U;
        int index;

        assert(wavefront_init(&sync) == 0);
        for (index = 0; index < TEST_ROWS; ++index) {
            creation_order[index] = index;
            arguments[index].sync = &sync;
            arguments[index].row = index;
            arguments[index].random_state =
                ((uint32_t)run + 1U) * 97U + (uint32_t)index;
            arguments[index].error = 0;
        }
        for (index = TEST_ROWS - 1; index > 0; --index) {
            int swap_with = (int)(next_random(&shuffle_state) %
                                  (uint32_t)(index + 1));
            int temporary = creation_order[index];

            creation_order[index] = creation_order[swap_with];
            creation_order[swap_with] = temporary;
        }

        for (index = 0; index < TEST_ROWS; ++index) {
            int row = creation_order[index];

            assert(pthread_create(&threads[row], NULL, fake_row_worker,
                                  &arguments[row]) == 0);
        }
        for (index = 0; index < TEST_ROWS; ++index) {
            assert(pthread_join(threads[index], NULL) == 0);
            assert(arguments[index].error == 0);
            assert(sync.row_progress[index] == TEST_COLUMNS);
        }
        assert(wavefront_destroy(&sync) == 0);
    }

    alarm(0U);
    printf("wavefront dependency test passed (%d randomized runs)\n",
           TEST_RUNS);
    return 0;
}
