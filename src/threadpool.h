#ifndef THREADPOOL_H
#define THREADPOOL_H

#include "codec.h"
#include "wavefront.h"

#include <pthread.h>
#include <stddef.h>

typedef struct Job Job;
typedef void (*JobFunction)(Job *job);

struct Job {
    int frame_number;
    int row;
    int plane;
    Frame *frame;
    WavefrontSync *wavefront_sync;
    JobFunction function;
    void *context;
    int shutdown;
};

typedef struct JobQueue {
    Job *jobs;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} JobQueue;

typedef struct ThreadPool ThreadPool;

int queue_push(JobQueue *queue, Job job);
int queue_pop(JobQueue *queue, Job *job);

ThreadPool *threadpool_create(int num_threads);
int threadpool_submit(ThreadPool *pool, Job job);
int threadpool_destroy(ThreadPool *pool);

/* Encodes and reconstructs one complete 8x8 luma-block row. */
int encode_row(Frame *frame, int row, WavefrontSync *sync);

#endif
