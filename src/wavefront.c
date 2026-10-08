#include "wavefront.h"

#include <errno.h>
#include <string.h>

int wavefront_init(WavefrontSync *sync)
{
    int result;

    if (sync == NULL) {
        return EINVAL;
    }
    memset(sync->row_progress, 0, sizeof sync->row_progress);

    result = pthread_mutex_init(&sync->mutex, NULL);
    if (result != 0) {
        return result;
    }
    result = pthread_cond_init(&sync->condition, NULL);
    if (result != 0) {
        pthread_mutex_destroy(&sync->mutex);
        return result;
    }
    return 0;
}

int wavefront_destroy(WavefrontSync *sync)
{
    int condition_result;
    int mutex_result;

    if (sync == NULL) {
        return EINVAL;
    }
    condition_result = pthread_cond_destroy(&sync->condition);
    mutex_result = pthread_mutex_destroy(&sync->mutex);
    return condition_result != 0 ? condition_result : mutex_result;
}

int wavefront_wait_for_dependency(WavefrontSync *sync, int row, int col)
{
    int result;

    if (sync == NULL || row < 0 || row >= MAX_ROWS || col < 0) {
        return EINVAL;
    }
    if (row == 0) {
        return 0;
    }

    result = pthread_mutex_lock(&sync->mutex);
    if (result != 0) {
        return result;
    }
    while (sync->row_progress[row - 1] <= col) {
        result = pthread_cond_wait(&sync->condition, &sync->mutex);
        if (result != 0) {
            pthread_mutex_unlock(&sync->mutex);
            return result;
        }
    }
    return pthread_mutex_unlock(&sync->mutex);
}

int wavefront_mark_done(WavefrontSync *sync, int row, int col)
{
    int result;
    int broadcast_result;
    int unlock_result;

    if (sync == NULL || row < 0 || row >= MAX_ROWS || col < 0) {
        return EINVAL;
    }

    result = pthread_mutex_lock(&sync->mutex);
    if (result != 0) {
        return result;
    }
    if (sync->row_progress[row] < col + 1) {
        sync->row_progress[row] = col + 1;
    }
    broadcast_result = pthread_cond_broadcast(&sync->condition);
    unlock_result = pthread_mutex_unlock(&sync->mutex);
    return broadcast_result != 0 ? broadcast_result : unlock_result;
}
