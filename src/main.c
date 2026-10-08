#include "codec.h"
#include "reorder.h"
#include "threadpool.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct EncodedBlock {
    uint8_t pair_count;
    RLEPair pairs[DCT_BLOCK_SIZE * DCT_BLOCK_SIZE];
} EncodedBlock;

typedef struct PipelineState {
    pthread_mutex_t mutex;
    int error_code;
    ReorderBuffer *reorder;
} PipelineState;

typedef struct FrameWork {
    Frame frame;
    WavefrontSync sync[3];
    int sync_count;
    EncodedBlock *blocks;
    uint32_t block_count;
    uint32_t plane_offset[3];
    int remaining_rows;
    pthread_mutex_t completion_mutex;
    int completion_mutex_ready;
    PipelineState *pipeline;
} FrameWork;

static int parse_positive(const char *text, int *value)
{
    char *end;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || parsed <= 0 ||
        parsed > INT_MAX) {
        return 0;
    }
    *value = (int)parsed;
    return 1;
}

static Block flat_prediction(int value)
{
    Block block;
    int row;
    int column;

    for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
        for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
            block.values[row][column] = value;
        }
    }
    return block;
}

static void pipeline_fail(PipelineState *pipeline, int error_code)
{
    pthread_mutex_lock(&pipeline->mutex);
    if (pipeline->error_code == 0) {
        pipeline->error_code = error_code == 0 ? EIO : error_code;
        reorder_abort(pipeline->reorder, pipeline->error_code);
    }
    pthread_mutex_unlock(&pipeline->mutex);
}

static void destroy_frame_work(FrameWork *work)
{
    int index;

    if (work == NULL) {
        return;
    }
    for (index = 0; index < work->sync_count; ++index) {
        wavefront_destroy(&work->sync[index]);
    }
    if (work->completion_mutex_ready) {
        pthread_mutex_destroy(&work->completion_mutex);
    }
    free(work->frame.orig);
    free(work->frame.recon);
    free(work->blocks);
    free(work);
}

static int finalize_frame(FrameWork *work)
{
    RLEPair *all_pairs;
    HuffmanBitstream encoded = {0};
    size_t total_pairs = 0U;
    size_t output_index = 0U;
    uint32_t block;
    int result;

    for (block = 0U; block < work->block_count; ++block) {
        total_pairs += work->blocks[block].pair_count;
    }
    all_pairs = malloc(total_pairs * sizeof *all_pairs);
    if (all_pairs == NULL) {
        return ENOMEM;
    }
    for (block = 0U; block < work->block_count; ++block) {
        size_t count = work->blocks[block].pair_count;

        memcpy(all_pairs + output_index, work->blocks[block].pairs,
               count * sizeof *all_pairs);
        output_index += count;
    }

    if (!huffman_encode(all_pairs, total_pairs, &encoded)) {
        result = errno == 0 ? EIO : errno;
        free(all_pairs);
        return result;
    }
    free(all_pairs);

    result = reorder_submit(work->pipeline->reorder,
                            work->frame.frame_number,
                            work->block_count, &encoded);
    free_huffman_bitstream(&encoded);
    return result;
}

static int encode_plane_row(FrameWork *work, FramePlane plane, int block_row)
{
    Frame view;
    uint8_t *orig;
    int block_columns;
    int block_column;

    orig = frame_plane(&work->frame, FRAME_BUFFER_ORIG, plane);
    view.recon = frame_plane(&work->frame, FRAME_BUFFER_RECON, plane);
    if (orig == NULL || view.recon == NULL) {
        return EINVAL;
    }
    view.frame_number = work->frame.frame_number;
    view.width = plane == FRAME_PLANE_Y ? work->frame.width :
                                         work->frame.width / 2;
    view.height = plane == FRAME_PLANE_Y ? work->frame.height :
                                          work->frame.height / 2;
    view.orig = orig;
    block_columns = view.width / DCT_BLOCK_SIZE;

    for (block_column = 0; block_column < block_columns; ++block_column) {
        int prediction_value;
        Block predicted;
        Block encoded_residual;
        int spatial_residual[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
        double transformed[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
        int quantized[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
        RLEPair pairs[DCT_BLOCK_SIZE * DCT_BLOCK_SIZE];
        size_t pair_count;
        uint32_t block_index;
        int row;
        int column;
        int result;

        result = wavefront_wait_for_dependency(
            &work->sync[plane], block_row, block_column);
        if (result != 0) {
            return result;
        }

        prediction_value = predict_from_neighbors(
            &view, block_row, block_column);
        predicted = flat_prediction(prediction_value);
        for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
            for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
                int frame_row = block_row * DCT_BLOCK_SIZE + row;
                int frame_column = block_column * DCT_BLOCK_SIZE + column;

                spatial_residual[row][column] =
                    orig[frame_row * view.width + frame_column] -
                    prediction_value;
            }
        }

        dct_transform(
            (const int (*)[DCT_BLOCK_SIZE])spatial_residual, transformed);
        quantize_block(
            (const double (*)[DCT_BLOCK_SIZE])transformed, quantized);
        pair_count = rle_encode(
            (const int (*)[DCT_BLOCK_SIZE])quantized, pairs);
        block_index = work->plane_offset[plane] +
                      (uint32_t)(block_row * block_columns + block_column);
        work->blocks[block_index].pair_count = (uint8_t)pair_count;
        memcpy(work->blocks[block_index].pairs, pairs,
               pair_count * sizeof *pairs);

        for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
            for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
                encoded_residual.values[row][column] = quantized[row][column];
            }
        }
        reconstruct_block(&view, block_row, block_column,
                          predicted, encoded_residual);

        if ((block_column + 1) % SYNC_GRANULARITY == 0 ||
            block_column + 1 == block_columns) {
            result = wavefront_mark_done(
                &work->sync[plane], block_row, block_column);
            if (result != 0) {
                return result;
            }
        }
    }
    return 0;
}

static void encode_pipeline_row(Job *job)
{
    FrameWork *work = job->context;
    int result = encode_plane_row(work, (FramePlane)job->plane, job->row);
    int is_last;

    if (result != 0) {
        pipeline_fail(work->pipeline, result);
    }

    pthread_mutex_lock(&work->completion_mutex);
    --work->remaining_rows;
    is_last = work->remaining_rows == 0;
    pthread_mutex_unlock(&work->completion_mutex);

    if (is_last) {
        if (result == 0) {
            result = finalize_frame(work);
            if (result != 0) {
                pipeline_fail(work->pipeline, result);
            }
        }
    }
}

static FrameWork *create_frame_work(FILE *input, int frame_number,
                                    int width, int height,
                                    PipelineState *pipeline)
{
    FrameWork *work = calloc(1U, sizeof *work);
    uint32_t y_blocks;
    uint32_t chroma_blocks;
    int index;

    if (work == NULL) {
        errno = ENOMEM;
        return NULL;
    }
    work->frame.frame_number = frame_number;
    work->frame.width = width;
    work->frame.height = height;
    work->pipeline = pipeline;
    if (read_frame(input, &work->frame) != READ_FRAME_OK) {
        destroy_frame_work(work);
        errno = EIO;
        return NULL;
    }

    y_blocks = (uint32_t)(width / DCT_BLOCK_SIZE) *
               (uint32_t)(height / DCT_BLOCK_SIZE);
    chroma_blocks = (uint32_t)(width / (2 * DCT_BLOCK_SIZE)) *
                    (uint32_t)(height / (2 * DCT_BLOCK_SIZE));
    work->plane_offset[FRAME_PLANE_Y] = 0U;
    work->plane_offset[FRAME_PLANE_U] = y_blocks;
    work->plane_offset[FRAME_PLANE_V] = y_blocks + chroma_blocks;
    work->block_count = y_blocks + 2U * chroma_blocks;
    work->remaining_rows = height / DCT_BLOCK_SIZE +
                           2 * (height / (2 * DCT_BLOCK_SIZE));
    work->blocks = calloc(work->block_count, sizeof *work->blocks);
    if (work->blocks == NULL) {
        destroy_frame_work(work);
        errno = ENOMEM;
        return NULL;
    }
    if (pthread_mutex_init(&work->completion_mutex, NULL) != 0) {
        destroy_frame_work(work);
        errno = EIO;
        return NULL;
    }
    work->completion_mutex_ready = 1;
    for (index = 0; index < 3; ++index) {
        int result = wavefront_init(&work->sync[index]);

        if (result != 0) {
            destroy_frame_work(work);
            errno = result;
            return NULL;
        }
        ++work->sync_count;
    }
    return work;
}

int main(int argc, char **argv)
{
    FILE *input = NULL;
    FILE *output = NULL;
    FrameWork **frames = NULL;
    ThreadPool *pool = NULL;
    PipelineState pipeline = {0};
    size_t frame_size;
    size_t frame_count;
    long input_size;
    int width;
    int height;
    int thread_count = 4;
    int status = EXIT_FAILURE;
    size_t frame_index;
    int plane;
    int row;

    if ((argc != 5 && argc != 6) ||
        !parse_positive(argv[3], &width) ||
        !parse_positive(argv[4], &height) ||
        width % 16 != 0 || height % 16 != 0 ||
        (argc == 6 && !parse_positive(argv[5], &thread_count))) {
        fprintf(stderr,
                "usage: %s input.yuv output.bin width height [threads]\n"
                "width and height must be positive multiples of 16\n",
                argv[0]);
        return EXIT_FAILURE;
    }
    if ((size_t)width > SIZE_MAX / (size_t)height) {
        fprintf(stderr, "frame dimensions are too large\n");
        return EXIT_FAILURE;
    }
    frame_size = (size_t)width * (size_t)height;
    if (frame_size > SIZE_MAX - frame_size / 2U) {
        fprintf(stderr, "frame dimensions are too large\n");
        return EXIT_FAILURE;
    }
    frame_size += frame_size / 2U;

    input = fopen(argv[1], "rb");
    if (input == NULL) {
        perror(argv[1]);
        goto cleanup;
    }
    if (fseek(input, 0L, SEEK_END) != 0 ||
        (input_size = ftell(input)) < 0 ||
        fseek(input, 0L, SEEK_SET) != 0 ||
        (size_t)input_size % frame_size != 0U) {
        fprintf(stderr, "input is not a whole number of YUV420 frames\n");
        goto cleanup;
    }
    frame_count = (size_t)input_size / frame_size;
    if (frame_count == 0U || frame_count > INT_MAX) {
        fprintf(stderr, "input contains no frames or too many frames\n");
        goto cleanup;
    }

    output = fopen(argv[2], "wb");
    if (output == NULL) {
        perror(argv[2]);
        goto cleanup;
    }
    if (pthread_mutex_init(&pipeline.mutex, NULL) != 0) {
        fprintf(stderr, "could not initialize pipeline mutex\n");
        goto cleanup;
    }
    pipeline.reorder = reorder_create(output, frame_count);
    if (pipeline.reorder == NULL) {
        perror("creating reorder buffer");
        pthread_mutex_destroy(&pipeline.mutex);
        goto cleanup;
    }

    frames = calloc(frame_count, sizeof *frames);
    if (frames == NULL) {
        pipeline_fail(&pipeline, ENOMEM);
        goto finish_reorder;
    }
    for (frame_index = 0U; frame_index < frame_count; ++frame_index) {
        frames[frame_index] = create_frame_work(
            input, (int)frame_index, width, height, &pipeline);
        if (frames[frame_index] == NULL) {
            perror("reading frame");
            pipeline_fail(&pipeline, errno);
            goto finish_reorder;
        }
    }

    pool = threadpool_create(thread_count);
    if (pool == NULL) {
        perror("creating thread pool");
        pipeline_fail(&pipeline, errno);
        goto finish_reorder;
    }

    for (plane = FRAME_PLANE_Y; plane <= FRAME_PLANE_V; ++plane) {
        int plane_height = plane == FRAME_PLANE_Y ? height : height / 2;

        for (row = 0; row < plane_height / DCT_BLOCK_SIZE; ++row) {
            for (frame_index = 0U; frame_index < frame_count; ++frame_index) {
                Job job = {
                    .frame_number = (int)frame_index,
                    .row = row,
                    .frame = &frames[frame_index]->frame,
                    .wavefront_sync = &frames[frame_index]->sync[plane],
                    .function = encode_pipeline_row,
                    .context = frames[frame_index],
                    .plane = plane
                };
                int result = threadpool_submit(pool, job);

                if (result != 0) {
                    pipeline_fail(&pipeline, result);
                    goto destroy_pool;
                }
            }
        }
    }

destroy_pool:
    {
        int result = threadpool_destroy(pool);
        pool = NULL;
        if (result != 0) {
            pipeline_fail(&pipeline, result);
        }
    }

finish_reorder:
    {
        int result = reorder_finish(pipeline.reorder);
        pipeline.reorder = NULL;
        if (result != 0 && pipeline.error_code == 0) {
            pipeline.error_code = result;
        }
    }
    pthread_mutex_destroy(&pipeline.mutex);
    if (frames != NULL) {
        for (frame_index = 0U; frame_index < frame_count; ++frame_index) {
            destroy_frame_work(frames[frame_index]);
            frames[frame_index] = NULL;
        }
    }
    if (pipeline.error_code == 0) {
        printf("encoded %zu frame%s with %d threads "
               "(sync granularity %d)\n",
               frame_count, frame_count == 1U ? "" : "s", thread_count,
               SYNC_GRANULARITY);
        status = EXIT_SUCCESS;
    } else {
        errno = pipeline.error_code;
        perror("encoding pipeline");
    }

cleanup:
    free(frames);
    if (input != NULL) {
        fclose(input);
    }
    if (output != NULL) {
        fclose(output);
    }
    return status;
}
