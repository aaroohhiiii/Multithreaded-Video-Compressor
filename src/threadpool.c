#include "threadpool.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

struct ThreadPool {
    pthread_t *threads;
    int thread_count;
    JobQueue queue;
};

static int queue_init(JobQueue *queue, size_t capacity)
{
    int result;

    if (queue == NULL || capacity == 0U ||
        capacity > SIZE_MAX / sizeof *queue->jobs) {
        return EINVAL;
    }
    queue->jobs = calloc(capacity, sizeof *queue->jobs);
    if (queue->jobs == NULL) {
        return ENOMEM;
    }
    queue->capacity = capacity;
    queue->head = 0U;
    queue->tail = 0U;
    queue->count = 0U;

    result = pthread_mutex_init(&queue->mutex, NULL);
    if (result != 0) {
        free(queue->jobs);
        queue->jobs = NULL;
        return result;
    }
    result = pthread_cond_init(&queue->not_empty, NULL);
    if (result != 0) {
        pthread_mutex_destroy(&queue->mutex);
        free(queue->jobs);
        queue->jobs = NULL;
        return result;
    }
    result = pthread_cond_init(&queue->not_full, NULL);
    if (result != 0) {
        pthread_cond_destroy(&queue->not_empty);
        pthread_mutex_destroy(&queue->mutex);
        free(queue->jobs);
        queue->jobs = NULL;
        return result;
    }
    return 0;
}

static int queue_destroy(JobQueue *queue)
{
    int not_empty_result;
    int not_full_result;
    int mutex_result;

    not_empty_result = pthread_cond_destroy(&queue->not_empty);
    not_full_result = pthread_cond_destroy(&queue->not_full);
    mutex_result = pthread_mutex_destroy(&queue->mutex);
    free(queue->jobs);
    queue->jobs = NULL;
    return not_empty_result != 0 ? not_empty_result :
           not_full_result != 0 ? not_full_result : mutex_result;
}

int queue_push(JobQueue *queue, Job job)
{
    int result;
    int unlock_result;

    if (queue == NULL || queue->jobs == NULL || queue->capacity == 0U) {
        return EINVAL;
    }
    result = pthread_mutex_lock(&queue->mutex);
    if (result != 0) {
        return result;
    }
    while (queue->count == queue->capacity) {
        result = pthread_cond_wait(&queue->not_full, &queue->mutex);
        if (result != 0) {
            pthread_mutex_unlock(&queue->mutex);
            return result;
        }
    }

    queue->jobs[queue->tail] = job;
    queue->tail = (queue->tail + 1U) % queue->capacity;
    ++queue->count;
    result = pthread_cond_signal(&queue->not_empty);
    unlock_result = pthread_mutex_unlock(&queue->mutex);
    return result != 0 ? result : unlock_result;
}

int queue_pop(JobQueue *queue, Job *job)
{
    int result;
    int unlock_result;

    if (queue == NULL || job == NULL || queue->jobs == NULL ||
        queue->capacity == 0U) {
        return EINVAL;
    }
    result = pthread_mutex_lock(&queue->mutex);
    if (result != 0) {
        return result;
    }
    while (queue->count == 0U) {
        result = pthread_cond_wait(&queue->not_empty, &queue->mutex);
        if (result != 0) {
            pthread_mutex_unlock(&queue->mutex);
            return result;
        }
    }

    *job = queue->jobs[queue->head];
    queue->head = (queue->head + 1U) % queue->capacity;
    --queue->count;
    result = pthread_cond_signal(&queue->not_full);
    unlock_result = pthread_mutex_unlock(&queue->mutex);
    return result != 0 ? result : unlock_result;
}

static void *worker_loop(void *argument)
{
    ThreadPool *pool = argument;

    for (;;) {
        Job job;

        if (queue_pop(&pool->queue, &job) != 0) {
            return NULL;
        }
        if (job.shutdown) {
            return NULL;
        }
        if (job.function != NULL) {
            job.function(&job);
        } else {
            encode_row(job.frame, job.row, job.wavefront_sync);
        }
    }
}

ThreadPool *threadpool_create(int num_threads)
{
    ThreadPool *pool;
    size_t queue_capacity;
    int result;
    int index;

    if (num_threads <= 0 || num_threads > INT32_MAX / 4) {
        errno = EINVAL;
        return NULL;
    }
    queue_capacity = (size_t)num_threads * 4U;
    pool = calloc(1U, sizeof *pool);
    if (pool == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    pool->threads = calloc((size_t)num_threads, sizeof *pool->threads);
    if (pool->threads == NULL) {
        free(pool);
        errno = ENOMEM;
        return NULL;
    }
    result = queue_init(&pool->queue, queue_capacity);
    if (result != 0) {
        free(pool->threads);
        free(pool);
        errno = result;
        return NULL;
    }

    for (index = 0; index < num_threads; ++index) {
        result = pthread_create(&pool->threads[index], NULL,
                                worker_loop, pool);

        if (result != 0) {
            int created = index;
            int sentinel;

            for (sentinel = 0; sentinel < created; ++sentinel) {
                Job shutdown_job = {.shutdown = 1};
                queue_push(&pool->queue, shutdown_job);
            }
            for (sentinel = 0; sentinel < created; ++sentinel) {
                pthread_join(pool->threads[sentinel], NULL);
            }
            queue_destroy(&pool->queue);
            free(pool->threads);
            free(pool);
            errno = result;
            return NULL;
        }
        ++pool->thread_count;
    }
    return pool;
}

int threadpool_submit(ThreadPool *pool, Job job)
{
    if (pool == NULL || job.shutdown) {
        return EINVAL;
    }
    return queue_push(&pool->queue, job);
}

int threadpool_destroy(ThreadPool *pool)
{
    int first_error = 0;
    int index;

    if (pool == NULL) {
        return EINVAL;
    }
    for (index = 0; index < pool->thread_count; ++index) {
        Job shutdown_job = {.shutdown = 1};
        int result = queue_push(&pool->queue, shutdown_job);

        if (result != 0 && first_error == 0) {
            first_error = result;
        }
    }
    for (index = 0; index < pool->thread_count; ++index) {
        int result = pthread_join(pool->threads[index], NULL);

        if (result != 0 && first_error == 0) {
            first_error = result;
        }
    }
    {
        int result = queue_destroy(&pool->queue);

        if (result != 0 && first_error == 0) {
            first_error = result;
        }
    }
    free(pool->threads);
    free(pool);
    return first_error;
}

static Block flat_prediction(int value)
{
    Block prediction;
    int row;
    int column;

    for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
        for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
            prediction.values[row][column] = value;
        }
    }
    return prediction;
}

int encode_row(Frame *frame, int row, WavefrontSync *sync)
{
    uint8_t *orig;
    int block_columns;
    int column;

    if (frame == NULL || sync == NULL || row < 0 ||
        frame->width <= 0 || frame->height <= 0 ||
        frame->width % DCT_BLOCK_SIZE != 0 ||
        frame->height % DCT_BLOCK_SIZE != 0 ||
        row >= frame->height / DCT_BLOCK_SIZE) {
        return EINVAL;
    }
    orig = frame_plane(frame, FRAME_BUFFER_ORIG, FRAME_PLANE_Y);
    if (orig == NULL ||
        frame_plane(frame, FRAME_BUFFER_RECON, FRAME_PLANE_Y) == NULL) {
        return EINVAL;
    }

    block_columns = frame->width / DCT_BLOCK_SIZE;
    for (column = 0; column < block_columns; ++column) {
        int prediction_value;
        Block predicted;
        Block encoded_residual;
        int spatial_residual[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
        double transformed[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
        int quantized[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
        RLEPair pairs[DCT_BLOCK_SIZE * DCT_BLOCK_SIZE];
        HuffmanBitstream entropy = {0};
        size_t pair_count;
        int pixel_row;
        int pixel_column;
        int result;

        result = wavefront_wait_for_dependency(sync, row, column);
        if (result != 0) {
            return result;
        }

        prediction_value = predict_from_neighbors(frame, row, column);
        predicted = flat_prediction(prediction_value);
        for (pixel_row = 0; pixel_row < DCT_BLOCK_SIZE; ++pixel_row) {
            for (pixel_column = 0; pixel_column < DCT_BLOCK_SIZE;
                 ++pixel_column) {
                int frame_row = row * DCT_BLOCK_SIZE + pixel_row;
                int frame_column =
                    column * DCT_BLOCK_SIZE + pixel_column;

                spatial_residual[pixel_row][pixel_column] =
                    orig[frame_row * frame->width + frame_column] -
                    prediction_value;
            }
        }

        dct_transform(
            (const int (*)[DCT_BLOCK_SIZE])spatial_residual, transformed);
        quantize_block(
            (const double (*)[DCT_BLOCK_SIZE])transformed, quantized);
        pair_count = rle_encode(
            (const int (*)[DCT_BLOCK_SIZE])quantized, pairs);
        if (!huffman_encode(pairs, pair_count, &entropy)) {
            return errno == 0 ? EIO : errno;
        }

        for (pixel_row = 0; pixel_row < DCT_BLOCK_SIZE; ++pixel_row) {
            for (pixel_column = 0; pixel_column < DCT_BLOCK_SIZE;
                 ++pixel_column) {
                encoded_residual.values[pixel_row][pixel_column] =
                    quantized[pixel_row][pixel_column];
            }
        }
        reconstruct_block(frame, row, column, predicted, encoded_residual);
        free_huffman_bitstream(&entropy);

        if ((column + 1) % SYNC_GRANULARITY == 0 ||
            column + 1 == block_columns) {
            result = wavefront_mark_done(sync, row, column);
            if (result != 0) {
                return result;
            }
        }
    }
    return 0;
}
