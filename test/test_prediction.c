#include "codec.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fill_block(Frame *frame, int block_row, int block_col, int value)
{
    uint8_t *recon = frame_plane(frame, FRAME_BUFFER_RECON, FRAME_PLANE_Y);
    int start_row = block_row * DCT_BLOCK_SIZE;
    int start_column = block_col * DCT_BLOCK_SIZE;
    int row;
    int column;

    assert(recon != NULL);
    for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
        for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
            recon[(start_row + row) * frame->width + start_column + column] =
                (uint8_t)value;
        }
    }
}

int main(void)
{
    enum { FRAME_SIZE = 16 * 16 + 2 * 8 * 8 };
    Frame frame = {.frame_number = 0, .width = 16, .height = 16};
    const int block_values[2][2] = {{100, 102}, {98, 101}};
    const int expected_predictions[2][2] = {{128, 100}, {100, 100}};
    const int expected_residuals[2][2] = {{-28, 2}, {-2, 1}};
    int row;
    int column;

    frame.orig = malloc(FRAME_SIZE);
    frame.recon = calloc(FRAME_SIZE, 1U);
    assert(frame.orig != NULL && frame.recon != NULL);

    /* A read from orig would produce the wrong prediction in this test. */
    memset(frame.orig, 250, FRAME_SIZE);

    for (row = 0; row < 2; ++row) {
        for (column = 0; column < 2; ++column) {
            int prediction = predict_from_neighbors(&frame, row, column);
            int residual = block_values[row][column] - prediction;

            assert(prediction == expected_predictions[row][column]);
            assert(residual == expected_residuals[row][column]);
            printf("block (%d,%d): prediction=%d residual=%d\n",
                   row, column, prediction, residual);
            fill_block(&frame, row, column, block_values[row][column]);
        }
    }

    free(frame.orig);
    free(frame.recon);
    return 0;
}
