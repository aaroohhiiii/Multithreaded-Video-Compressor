#include "threadpool.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { TEST_RUNS = 12 };

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

static void encode_plain(Frame *frame)
{
    uint8_t *orig = frame_plane(frame, FRAME_BUFFER_ORIG, FRAME_PLANE_Y);
    int block_row;
    int block_column;

    assert(orig != NULL);
    for (block_row = 0; block_row < frame->height / DCT_BLOCK_SIZE;
         ++block_row) {
        for (block_column = 0;
             block_column < frame->width / DCT_BLOCK_SIZE;
             ++block_column) {
            int prediction_value = predict_from_neighbors(
                frame, block_row, block_column);
            Block predicted = flat_prediction(prediction_value);
            Block encoded_residual;
            int spatial_residual[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
            double transformed[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
            int quantized[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
            RLEPair pairs[DCT_BLOCK_SIZE * DCT_BLOCK_SIZE];
            HuffmanBitstream entropy = {0};
            size_t pair_count;
            int row;
            int column;

            for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
                for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
                    int frame_row = block_row * DCT_BLOCK_SIZE + row;
                    int frame_column =
                        block_column * DCT_BLOCK_SIZE + column;

                    spatial_residual[row][column] =
                        orig[frame_row * frame->width + frame_column] -
                        prediction_value;
                }
            }
            dct_transform(
                (const int (*)[DCT_BLOCK_SIZE])spatial_residual,
                transformed);
            quantize_block(
                (const double (*)[DCT_BLOCK_SIZE])transformed, quantized);
            pair_count = rle_encode(
                (const int (*)[DCT_BLOCK_SIZE])quantized, pairs);
            assert(huffman_encode(pairs, pair_count, &entropy));

            for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
                for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
                    encoded_residual.values[row][column] =
                        quantized[row][column];
                }
            }
            reconstruct_block(frame, block_row, block_column,
                              predicted, encoded_residual);
            free_huffman_bitstream(&entropy);
        }
    }
}

static void run_threaded(Frame *frame, const Frame *baseline,
                         size_t frame_size, int worker_count)
{
    int run;

    for (run = 0; run < TEST_RUNS; ++run) {
        WavefrontSync sync;
        ThreadPool *pool;
        int row;

        memset(frame->recon, 0, frame_size);
        assert(wavefront_init(&sync) == 0);
        pool = threadpool_create(worker_count);
        assert(pool != NULL);

        for (row = 0; row < frame->height / DCT_BLOCK_SIZE; ++row) {
            Job job = {
                .frame_number = frame->frame_number,
                .row = row,
                .frame = frame,
                .wavefront_sync = &sync
            };

            assert(threadpool_submit(pool, job) == 0);
        }
        assert(threadpool_destroy(pool) == 0);
        assert(memcmp(frame->recon, baseline->recon, frame_size) == 0);
        for (row = 0; row < frame->height / DCT_BLOCK_SIZE; ++row) {
            assert(sync.row_progress[row] ==
                   frame->width / DCT_BLOCK_SIZE);
        }
        assert(wavefront_destroy(&sync) == 0);
    }

    printf("%d-worker output matches baseline byte for byte (%d runs)\n",
           worker_count, TEST_RUNS);
}

int main(void)
{
    enum { WIDTH = 32, HEIGHT = 64, FRAME_SIZE = WIDTH * HEIGHT * 3 / 2 };
    Frame threaded = {.frame_number = 7, .width = WIDTH, .height = HEIGHT};
    Frame plain = {.frame_number = 7, .width = WIDTH, .height = HEIGHT};
    int index;

#ifndef HELGRIND_TEST
    alarm(30U);
#endif

    threaded.orig = malloc(FRAME_SIZE);
    threaded.recon = calloc(FRAME_SIZE, 1U);
    plain.orig = malloc(FRAME_SIZE);
    plain.recon = calloc(FRAME_SIZE, 1U);
    assert(threaded.orig != NULL && threaded.recon != NULL &&
           plain.orig != NULL && plain.recon != NULL);

    for (index = 0; index < FRAME_SIZE; ++index) {
        threaded.orig[index] =
            (uint8_t)((index * 31 + index / WIDTH * 11 + 17) & 0xff);
    }
    memcpy(plain.orig, threaded.orig, FRAME_SIZE);
    encode_plain(&plain);

    run_threaded(&threaded, &plain, FRAME_SIZE, 1);
    run_threaded(&threaded, &plain, FRAME_SIZE, 2);
    run_threaded(&threaded, &plain, FRAME_SIZE, 4);
#ifndef HELGRIND_TEST
    alarm(0U);
#endif
    free(threaded.orig);
    free(threaded.recon);
    free(plain.orig);
    free(plain.recon);
    return 0;
}
