#ifndef CODEC_H
#define CODEC_H

#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#define DCT_BLOCK_SIZE 8

#ifndef ENABLE_PREDICTION
#define ENABLE_PREDICTION 1
#endif

#if ENABLE_PREDICTION != 0 && ENABLE_PREDICTION != 1
#error "ENABLE_PREDICTION must be 0 or 1"
#endif

typedef enum {
    FRAME_PLANE_Y,
    FRAME_PLANE_U,
    FRAME_PLANE_V
} FramePlane;

typedef enum {
    FRAME_BUFFER_ORIG,
    FRAME_BUFFER_RECON
} FrameBuffer;

typedef struct Frame {
    int frame_number;
    int width;
    int height;

    /* Y, U, and V are packed contiguously in YUV420 order. */
    uint8_t *orig;
    uint8_t *recon;
} Frame;

typedef struct Block {
    double values[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
} Block;

typedef struct RLEPair {
    uint8_t zero_count;
    int32_t value;
} RLEPair;

typedef struct HuffmanBitstream {
    RLEPair *symbols;
    uint32_t *frequencies;
    size_t symbol_count;
    uint8_t *data;
    size_t bit_count;
} HuffmanBitstream;

typedef enum {
    READ_FRAME_ERROR = -1,
    READ_FRAME_EOF = 0,
    READ_FRAME_OK = 1
} ReadFrameResult;

/*
 * Returns the start of a plane in the selected buffer. This is the sole
 * interface responsible for calculating YUV420 plane offsets.
 *
 * During encoding, callers must treat FRAME_BUFFER_ORIG as read-only.
 */
uint8_t *frame_plane(Frame *frame, FrameBuffer buffer, FramePlane plane);

/*
 * Reads one complete YUV420 frame. width and height must be positive and even.
 * If orig or recon is NULL, storage for that buffer is allocated. Non-NULL
 * buffers must already have enough space for one frame. The caller owns both
 * buffers and must free them when the Frame is no longer needed.
 *
 * Returns READ_FRAME_EOF only when no bytes remain before a new frame starts.
 * A truncated frame or invalid argument returns READ_FRAME_ERROR.
 */
ReadFrameResult read_frame(FILE *input, Frame *frame);

/* Applies an orthonormal 8x8 DCT-II and its inverse. */
void dct_transform(const int input[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE],
                   double output[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE]);
void inverse_dct_transform(
    const double input[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE],
    double output[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE]);
void quantize_block(
    const double input[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE],
    int output[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE]);
void dequantize_block(
    const int input[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE],
    double output[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE]);

/*
 * Predicts an 8x8 luma block from reconstructed top/left boundary pixels.
 * block_row and block_col must identify a complete block within the frame.
 */
int predict_from_neighbors(Frame *frame, int block_row, int block_col);

/*
 * Reconstructs an 8x8 luma block. residual contains transform coefficients;
 * dequantization is currently the identity operation.
 */
void reconstruct_block(Frame *frame, int block_row, int block_col,
                       Block predicted, Block residual);

/* Returns the number of pairs written to output, whose capacity must be 64. */
size_t rle_encode(
    const int quantized[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE],
    RLEPair output[DCT_BLOCK_SIZE * DCT_BLOCK_SIZE]);

/* The returned allocations are released with free_huffman_bitstream(). */
int huffman_encode(const RLEPair *input, size_t input_count,
                   HuffmanBitstream *output);
void free_huffman_bitstream(HuffmanBitstream *bitstream);

/* Appends one self-contained frame record; returns 1 on success, 0 on error. */
int write_bitstream(FILE *output, int frame_number, uint32_t block_count,
                    const HuffmanBitstream *bitstream);

#endif
