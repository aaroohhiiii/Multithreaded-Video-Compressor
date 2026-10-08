#ifndef WAVEFRONT_H
#define WAVEFRONT_H

#include <pthread.h>

#define MAX_ROWS 4096

#ifndef SYNC_GRANULARITY
#define SYNC_GRANULARITY 1
#endif

#if SYNC_GRANULARITY < 1
#error "SYNC_GRANULARITY must be at least 1"
#endif

typedef struct WavefrontSync {
    int row_progress[MAX_ROWS];
    pthread_mutex_t mutex;
    pthread_cond_t condition;
} WavefrontSync;

int wavefront_init(WavefrontSync *sync);
int wavefront_destroy(WavefrontSync *sync);
int wavefront_wait_for_dependency(WavefrontSync *sync, int row, int col);
int wavefront_mark_done(WavefrontSync *sync, int row, int col);

#endif
