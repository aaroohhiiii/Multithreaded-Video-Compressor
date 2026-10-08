#include "codec.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void)
{
    static const int original[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE] = {
        {52, 55, 61, 66, 70, 61, 64, 73},
        {63, 59, 55, 90, 109, 85, 69, 72},
        {62, 59, 68, 113, 144, 104, 66, 73},
        {63, 58, 71, 122, 154, 106, 70, 69},
        {67, 61, 68, 104, 126, 88, 68, 70},
        {79, 65, 60, 70, 77, 68, 58, 75},
        {85, 71, 64, 59, 55, 61, 65, 83},
        {87, 79, 69, 68, 65, 76, 78, 94}
    };
    double coefficients[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
    double reconstructed[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
    double maximum_error = 0.0;
    int row;
    int column;

    dct_transform(original, coefficients);
    inverse_dct_transform(
        (const double (*)[DCT_BLOCK_SIZE])coefficients, reconstructed);

    for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
        for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
            double error = fabs(reconstructed[row][column] -
                                original[row][column]);

            if (error > maximum_error) {
                maximum_error = error;
            }
            assert(error < 1e-9);
        }
    }

    printf("DCT round-trip maximum error: %.12g\n", maximum_error);
    return 0;
}
