#include "reorder.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct ReorderSlot {
    int ready;
    uint32_t block_count;
    HuffmanBitstream bitstream;
} ReorderSlot;

struct ReorderBuffer {
    ReorderSlot *slots;
    size_t frame_count;
    size_t next_expected_frame;
    FILE *output;
    pthread_t writer;
    pthread_mutex_t mutex;
    pthread_cond_t ready;
    int aborted;
    int error_code;
};

static void *writer_loop(void *argument)
{
    ReorderBuffer *buffer = argument;

    for (;;) {
        HuffmanBitstream bitstream;
        uint32_t block_count;
        int frame_number;
        int write_ok;

        pthread_mutex_lock(&buffer->mutex);
        while (!buffer->aborted &&
               buffer->next_expected_frame < buffer->frame_count &&
               !buffer->slots[buffer->next_expected_frame].ready) {
            pthread_cond_wait(&buffer->ready, &buffer->mutex);
        }
        if (buffer->aborted ||
            buffer->next_expected_frame == buffer->frame_count) {
            pthread_mutex_unlock(&buffer->mutex);
            return NULL;
        }

        frame_number = (int)buffer->next_expected_frame;
        block_count = buffer->slots[frame_number].block_count;
        bitstream = buffer->slots[frame_number].bitstream;
        memset(&buffer->slots[frame_number].bitstream, 0,
               sizeof buffer->slots[frame_number].bitstream);
        buffer->slots[frame_number].ready = 0;
        ++buffer->next_expected_frame;
        pthread_mutex_unlock(&buffer->mutex);

        write_ok = write_bitstream(buffer->output, frame_number,
                                   block_count, &bitstream);
        free_huffman_bitstream(&bitstream);
        if (!write_ok) {
            reorder_abort(buffer, errno == 0 ? EIO : errno);
            return NULL;
        }
    }
}

ReorderBuffer *reorder_create(FILE *output, size_t frame_count)
{
    ReorderBuffer *buffer;
    int result;

    if (output == NULL || frame_count == 0U || frame_count > INT32_MAX) {
        errno = EINVAL;
        return NULL;
    }
    buffer = calloc(1U, sizeof *buffer);
    if (buffer == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    buffer->slots = calloc(frame_count, sizeof *buffer->slots);
    if (buffer->slots == NULL) {
        free(buffer);
        errno = ENOMEM;
        return NULL;
    }
    buffer->frame_count = frame_count;
    buffer->output = output;

    result = pthread_mutex_init(&buffer->mutex, NULL);
    if (result != 0) {
        free(buffer->slots);
        free(buffer);
        errno = result;
        return NULL;
    }
    result = pthread_cond_init(&buffer->ready, NULL);
    if (result != 0) {
        pthread_mutex_destroy(&buffer->mutex);
        free(buffer->slots);
        free(buffer);
        errno = result;
        return NULL;
    }
    result = pthread_create(&buffer->writer, NULL, writer_loop, buffer);
    if (result != 0) {
        pthread_cond_destroy(&buffer->ready);
        pthread_mutex_destroy(&buffer->mutex);
        free(buffer->slots);
        free(buffer);
        errno = result;
        return NULL;
    }
    return buffer;
}

int reorder_submit(ReorderBuffer *buffer, int frame_number,
                   uint32_t block_count, HuffmanBitstream *bitstream)
{
    int result;

    if (buffer == NULL || bitstream == NULL || frame_number < 0 ||
        (size_t)frame_number >= buffer->frame_count) {
        return EINVAL;
    }
    result = pthread_mutex_lock(&buffer->mutex);
    if (result != 0) {
        return result;
    }
    if (buffer->aborted || buffer->slots[frame_number].ready ||
        (size_t)frame_number < buffer->next_expected_frame) {
        pthread_mutex_unlock(&buffer->mutex);
        return buffer->aborted ? ECANCELED : EINVAL;
    }

    buffer->slots[frame_number].block_count = block_count;
    buffer->slots[frame_number].bitstream = *bitstream;
    buffer->slots[frame_number].ready = 1;
    memset(bitstream, 0, sizeof *bitstream);
    result = pthread_cond_broadcast(&buffer->ready);
    {
        int unlock_result = pthread_mutex_unlock(&buffer->mutex);
        return result != 0 ? result : unlock_result;
    }
}

void reorder_abort(ReorderBuffer *buffer, int error_code)
{
    if (buffer == NULL) {
        return;
    }
    pthread_mutex_lock(&buffer->mutex);
    if (!buffer->aborted) {
        buffer->aborted = 1;
        buffer->error_code = error_code == 0 ? EIO : error_code;
    }
    pthread_cond_broadcast(&buffer->ready);
    pthread_mutex_unlock(&buffer->mutex);
}

int reorder_finish(ReorderBuffer *buffer)
{
    int result = 0;
    size_t index;

    if (buffer == NULL) {
        return EINVAL;
    }
    pthread_join(buffer->writer, NULL);
    if (buffer->aborted) {
        result = buffer->error_code;
    } else if (fflush(buffer->output) != 0) {
        result = errno == 0 ? EIO : errno;
    }

    for (index = 0U; index < buffer->frame_count; ++index) {
        free_huffman_bitstream(&buffer->slots[index].bitstream);
    }
    pthread_cond_destroy(&buffer->ready);
    pthread_mutex_destroy(&buffer->mutex);
    free(buffer->slots);
    free(buffer);
    return result;
}
