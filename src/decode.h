#ifndef DECODE_H
#define DECODE_H

#include "codec.h"

#include <stddef.h>
#include <stdio.h>

int huffman_decode(const HuffmanBitstream *input, RLEPair **output,
                   size_t *output_count);
int rle_decode(const RLEPair *input, size_t input_count,
               size_t *pairs_consumed,
               int output[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE]);

/* Decodes every frame record and returns the number of frames, or -1. */
int decode_stream(FILE *input, FILE *raw_output, int width, int height);

#endif
