#ifndef REORDER_H
#define REORDER_H

#include "codec.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef struct ReorderBuffer ReorderBuffer;

ReorderBuffer *reorder_create(FILE *output, size_t frame_count);

/* Takes ownership of bitstream's allocations on success. */
int reorder_submit(ReorderBuffer *buffer, int frame_number,
                   uint32_t block_count, HuffmanBitstream *bitstream);

void reorder_abort(ReorderBuffer *buffer, int error_code);

/* Waits for the writer, releases the buffer, and returns zero or an errno. */
int reorder_finish(ReorderBuffer *buffer);

#endif
