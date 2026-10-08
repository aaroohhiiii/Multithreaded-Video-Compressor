#include "threadpool.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

int main(void)
{
    enum { WIDTH = 32, HEIGHT = 24, FRAME_SIZE = WIDTH * HEIGHT * 3 / 2 };
    Frame wavefront_frame = {.width = WIDTH, .height = HEIGHT};
    Frame plain_frame = {.width = WIDTH, .height = HEIGHT};
    WavefrontSync sync;
    int index;
    int row;

    wavefront_frame.orig = malloc(FRAME_SIZE);
    wavefront_frame.recon = calloc(FRAME_SIZE, 1U);
    plain_frame.orig = malloc(FRAME_SIZE);
    plain_frame.recon = calloc(FRAME_SIZE, 1U);
    assert(wavefront_frame.orig != NULL && wavefront_frame.recon != NULL &&
           plain_frame.orig != NULL && plain_frame.recon != NULL);

    for (index = 0; index < FRAME_SIZE; ++index) {
        wavefront_frame.orig[index] =
            (uint8_t)((index * 29 + index / WIDTH * 7) & 0xff);
    }
    memcpy(plain_frame.orig, wavefront_frame.orig, FRAME_SIZE);

    encode_plain(&plain_frame);
    assert(wavefront_init(&sync) == 0);
    for (row = 0; row < HEIGHT / DCT_BLOCK_SIZE; ++row) {
        assert(encode_row(&wavefront_frame, row, &sync) == 0);
    }

    assert(memcmp(wavefront_frame.recon, plain_frame.recon, FRAME_SIZE) == 0);
    for (row = 0; row < HEIGHT / DCT_BLOCK_SIZE; ++row) {
        assert(sync.row_progress[row] == WIDTH / DCT_BLOCK_SIZE);
    }
    assert(wavefront_destroy(&sync) == 0);

    printf("encode_row matches plain traversal byte for byte (%dx%d luma)\n",
           WIDTH, HEIGHT);
    free(wavefront_frame.orig);
    free(wavefront_frame.recon);
    free(plain_frame.orig);
    free(plain_frame.recon);
    return 0;
}
