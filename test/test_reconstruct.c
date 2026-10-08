#include "codec.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

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

int main(void)
{
    enum { WIDTH = 16, HEIGHT = 16, FRAME_SIZE = WIDTH * HEIGHT * 3 / 2 };
    Frame frame = {.frame_number = 0, .width = WIDTH, .height = HEIGHT};
    uint8_t *orig_y;
    uint8_t *recon_y;
    int block_row;
    int block_column;
    int row;
    int column;
    int maximum_error = 0;

    frame.orig = malloc(FRAME_SIZE);
    frame.recon = calloc(FRAME_SIZE, 1U);
    assert(frame.orig != NULL && frame.recon != NULL);
    orig_y = frame_plane(&frame, FRAME_BUFFER_ORIG, FRAME_PLANE_Y);
    assert(orig_y != NULL);

    for (row = 0; row < HEIGHT; ++row) {
        for (column = 0; column < WIDTH; ++column) {
            orig_y[row * WIDTH + column] =
                (uint8_t)(20 + row * 3 + column * 2);
        }
    }

    for (block_row = 0; block_row < HEIGHT / DCT_BLOCK_SIZE; ++block_row) {
        for (block_column = 0;
             block_column < WIDTH / DCT_BLOCK_SIZE;
             ++block_column) {
            int prediction = predict_from_neighbors(
                &frame, block_row, block_column);
            Block predicted = flat_prediction(prediction);
            Block encoded_residual;
            int spatial_residual[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
            double transformed[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
            int quantized[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];

            for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
                for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
                    int frame_row = block_row * DCT_BLOCK_SIZE + row;
                    int frame_column =
                        block_column * DCT_BLOCK_SIZE + column;

                    spatial_residual[row][column] =
                        orig_y[frame_row * WIDTH + frame_column] - prediction;
                }
            }

            dct_transform(
                (const int (*)[DCT_BLOCK_SIZE])spatial_residual,
                transformed);
            quantize_block(
                (const double (*)[DCT_BLOCK_SIZE])transformed, quantized);
            for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
                for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
                    encoded_residual.values[row][column] =
                        quantized[row][column];
                }
            }
            reconstruct_block(&frame, block_row, block_column,
                              predicted, encoded_residual);
        }
    }

    recon_y = frame_plane(&frame, FRAME_BUFFER_RECON, FRAME_PLANE_Y);
    assert(recon_y != NULL);
    for (row = 0; row < HEIGHT; ++row) {
        for (column = 0; column < WIDTH; ++column) {
            int error = abs((int)recon_y[row * WIDTH + column] -
                            (int)orig_y[row * WIDTH + column]);

            if (error > maximum_error) {
                maximum_error = error;
            }
            assert(error <= 4);
        }
    }

    printf("quantized full-frame reconstruction passed (%dx%d, max error %d)\n",
           WIDTH, HEIGHT, maximum_error);
    free(frame.orig);
    free(frame.recon);
    return 0;
}
