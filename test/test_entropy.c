#include "codec.h"

#include <assert.h>
#include <stdio.h>

int main(void)
{
    int coefficients[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE] = {{0}};
    RLEPair pairs[DCT_BLOCK_SIZE * DCT_BLOCK_SIZE];
    HuffmanBitstream encoded = {0};
    size_t count;

    coefficients[0][0] = 12;
    coefficients[1][0] = -3; /* Third coefficient in zig-zag order. */
    count = rle_encode(
        (const int (*)[DCT_BLOCK_SIZE])coefficients, pairs);

    assert(count == 3U);
    assert(pairs[0].zero_count == 0U && pairs[0].value == 12);
    assert(pairs[1].zero_count == 1U && pairs[1].value == -3);
    assert(pairs[2].zero_count == 61U && pairs[2].value == 0);

    assert(huffman_encode(pairs, count, &encoded));
    assert(encoded.symbol_count == 3U);
    assert(encoded.bit_count > 0U);
    assert(encoded.bit_count < count * 8U);
    printf("RLE pairs: %zu, Huffman bits: %zu\n", count,
           encoded.bit_count);

    free_huffman_bitstream(&encoded);
    return 0;
}
