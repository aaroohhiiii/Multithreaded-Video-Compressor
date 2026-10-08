#include "codec.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const int quantization_table[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE] = {
    {16, 11, 10, 16, 24, 40, 51, 61},
    {12, 12, 14, 19, 26, 58, 60, 55},
    {14, 13, 16, 24, 40, 57, 69, 56},
    {14, 17, 22, 29, 51, 87, 80, 62},
    {18, 22, 37, 56, 68, 109, 103, 77},
    {24, 35, 55, 64, 81, 104, 113, 92},
    {49, 64, 78, 87, 103, 121, 120, 101},
    {72, 92, 95, 98, 112, 100, 103, 99}
};

static int frame_size(const Frame *frame, size_t *size)
{
    size_t luma_size;

    if (frame == NULL || size == NULL || frame->width <= 0 ||
        frame->height <= 0 || frame->width % 2 != 0 ||
        frame->height % 2 != 0) {
        errno = EINVAL;
        return 0;
    }

    if ((size_t)frame->width > SIZE_MAX / (size_t)frame->height) {
        errno = EOVERFLOW;
        return 0;
    }

    luma_size = (size_t)frame->width * (size_t)frame->height;
    if (luma_size > SIZE_MAX - luma_size / 2U) {
        errno = EOVERFLOW;
        return 0;
    }

    *size = luma_size + luma_size / 2U;
    return 1;
}

uint8_t *frame_plane(Frame *frame, FrameBuffer buffer, FramePlane plane)
{
    uint8_t *base;
    size_t luma_size;
    size_t chroma_size;

    if (frame == NULL || frame->width <= 0 || frame->height <= 0 ||
        frame->width % 2 != 0 || frame->height % 2 != 0) {
        return NULL;
    }

    if ((size_t)frame->width > SIZE_MAX / (size_t)frame->height) {
        return NULL;
    }

    if (buffer == FRAME_BUFFER_ORIG) {
        base = frame->orig;
    } else if (buffer == FRAME_BUFFER_RECON) {
        base = frame->recon;
    } else {
        return NULL;
    }

    if (base == NULL) {
        return NULL;
    }

    luma_size = (size_t)frame->width * (size_t)frame->height;
    chroma_size = luma_size / 4U;

    switch (plane) {
    case FRAME_PLANE_Y:
        return base;
    case FRAME_PLANE_U:
        return base + luma_size;
    case FRAME_PLANE_V:
        return base + luma_size + chroma_size;
    default:
        return NULL;
    }
}

ReadFrameResult read_frame(FILE *input, Frame *frame)
{
    uint8_t *orig;
    uint8_t *recon;
    uint8_t *new_orig = NULL;
    uint8_t *new_recon = NULL;
    size_t size;
    size_t bytes_read = 0U;

    if (input == NULL || !frame_size(frame, &size)) {
        if (input == NULL) {
            errno = EINVAL;
        }
        return READ_FRAME_ERROR;
    }

    orig = frame->orig;
    recon = frame->recon;

    if (orig == NULL) {
        new_orig = malloc(size);
        orig = new_orig;
    }
    if (recon == NULL) {
        new_recon = calloc(size, 1U);
        recon = new_recon;
    }
    if (orig == NULL || recon == NULL) {
        free(new_orig);
        free(new_recon);
        errno = ENOMEM;
        return READ_FRAME_ERROR;
    }

    memset(recon, 0, size);
    while (bytes_read < size) {
        size_t count = fread(orig + bytes_read, 1U, size - bytes_read, input);

        if (count == 0U) {
            break;
        }
        bytes_read += count;
    }

    if (bytes_read == 0U && feof(input)) {
        free(new_orig);
        free(new_recon);
        return READ_FRAME_EOF;
    }

    if (bytes_read != size) {
        free(new_orig);
        free(new_recon);
        if (!ferror(input)) {
            errno = EIO;
        }
        return READ_FRAME_ERROR;
    }

    frame->orig = orig;
    frame->recon = recon;
    return READ_FRAME_OK;
}

static void build_dct_basis(double basis[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE])
{
    const double pi = acos(-1.0);
    int frequency;
    int position;

    for (frequency = 0; frequency < DCT_BLOCK_SIZE; ++frequency) {
        double scale = frequency == 0 ? 1.0 / sqrt(8.0) : 0.5;

        for (position = 0; position < DCT_BLOCK_SIZE; ++position) {
            basis[frequency][position] =
                scale * cos(((2.0 * position + 1.0) * frequency * pi) /
                            16.0);
        }
    }
}

void dct_transform(const int input[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE],
                   double output[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE])
{
    double basis[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
    double row_result[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
    int row;
    int column;
    int frequency;
    int position;

    build_dct_basis(basis);

    for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
        for (frequency = 0; frequency < DCT_BLOCK_SIZE; ++frequency) {
            double sum = 0.0;

            for (position = 0; position < DCT_BLOCK_SIZE; ++position) {
                sum += input[row][position] * basis[frequency][position];
            }
            row_result[row][frequency] = sum;
        }
    }

    for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
        for (frequency = 0; frequency < DCT_BLOCK_SIZE; ++frequency) {
            double sum = 0.0;

            for (position = 0; position < DCT_BLOCK_SIZE; ++position) {
                sum += row_result[position][column] *
                       basis[frequency][position];
            }
            output[frequency][column] = sum;
        }
    }
}

void inverse_dct_transform(
    const double input[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE],
    double output[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE])
{
    double basis[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
    double column_result[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
    int row;
    int column;
    int frequency;
    int position;

    build_dct_basis(basis);

    for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
        for (position = 0; position < DCT_BLOCK_SIZE; ++position) {
            double sum = 0.0;

            for (frequency = 0; frequency < DCT_BLOCK_SIZE; ++frequency) {
                sum += input[frequency][column] *
                       basis[frequency][position];
            }
            column_result[position][column] = sum;
        }
    }

    for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
        for (position = 0; position < DCT_BLOCK_SIZE; ++position) {
            double sum = 0.0;

            for (frequency = 0; frequency < DCT_BLOCK_SIZE; ++frequency) {
                sum += column_result[row][frequency] *
                       basis[frequency][position];
            }
            output[row][position] = sum;
        }
    }
}

void quantize_block(
    const double input[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE],
    int output[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE])
{
    int row;
    int column;

    for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
        for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
            double value = input[row][column] /
                           quantization_table[row][column];

            if (value > INT_MAX) {
                output[row][column] = INT_MAX;
            } else if (value < INT_MIN) {
                output[row][column] = INT_MIN;
            } else {
                output[row][column] = (int)lround(value);
            }
        }
    }
}

int predict_from_neighbors(Frame *frame, int block_row, int block_col)
{
#if ENABLE_PREDICTION == 0
    (void)frame;
    (void)block_row;
    (void)block_col;
    return 128;
#else
    uint8_t *recon;
    int pixel_row;
    int pixel_column;
    int sum = 0;
    int sample_count = 0;
    int index;

    if (frame == NULL || block_row < 0 || block_col < 0 ||
        frame->height < DCT_BLOCK_SIZE || frame->width < DCT_BLOCK_SIZE ||
        block_row >= frame->height / DCT_BLOCK_SIZE ||
        block_col >= frame->width / DCT_BLOCK_SIZE) {
        return 128;
    }

    pixel_row = block_row * DCT_BLOCK_SIZE;
    pixel_column = block_col * DCT_BLOCK_SIZE;
    if (block_row == 0 && block_col == 0) {
        return 128;
    }

    recon = frame_plane(frame, FRAME_BUFFER_RECON, FRAME_PLANE_Y);
    if (recon == NULL) {
        return 128;
    }

    if (block_row > 0) {
        int top_boundary_row = pixel_row - 1;

        for (index = 0; index < DCT_BLOCK_SIZE; ++index) {
            sum += recon[top_boundary_row * frame->width +
                         pixel_column + index];
            ++sample_count;
        }
    }

    if (block_col > 0) {
        int left_boundary_column = pixel_column - 1;

        for (index = 0; index < DCT_BLOCK_SIZE; ++index) {
            sum += recon[(pixel_row + index) * frame->width +
                         left_boundary_column];
            ++sample_count;
        }
    }

    return sum / sample_count;
#endif
}

void dequantize_block(
    const int input[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE],
    double output[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE])
{
    int row;
    int column;

    for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
        for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
            output[row][column] =
                input[row][column] * quantization_table[row][column];
        }
    }
}

size_t rle_encode(
    const int quantized[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE],
    RLEPair output[DCT_BLOCK_SIZE * DCT_BLOCK_SIZE])
{
    static const uint8_t zigzag[DCT_BLOCK_SIZE * DCT_BLOCK_SIZE] = {
         0,  1,  8, 16,  9,  2,  3, 10,
        17, 24, 32, 25, 18, 11,  4,  5,
        12, 19, 26, 33, 40, 48, 41, 34,
        27, 20, 13,  6,  7, 14, 21, 28,
        35, 42, 49, 56, 57, 50, 43, 36,
        29, 22, 15, 23, 30, 37, 44, 51,
        58, 59, 52, 45, 38, 31, 39, 46,
        53, 60, 61, 54, 47, 55, 62, 63
    };
    size_t output_count = 0U;
    uint8_t zero_count = 0U;
    int index;

    for (index = 0; index < DCT_BLOCK_SIZE * DCT_BLOCK_SIZE; ++index) {
        int position = zigzag[index];
        int value = quantized[position / DCT_BLOCK_SIZE]
                             [position % DCT_BLOCK_SIZE];

        if (value == 0) {
            ++zero_count;
            continue;
        }

        output[output_count].zero_count = zero_count;
        output[output_count].value = value;
        ++output_count;
        zero_count = 0U;
    }

    if (zero_count > 0U) {
        output[output_count].zero_count = zero_count;
        output[output_count].value = 0;
        ++output_count;
    }

    return output_count;
}

typedef struct HuffmanNode {
    uint64_t frequency;
    int parent;
    int left;
    int right;
    int active;
} HuffmanNode;

typedef struct BitWriter {
    uint8_t *data;
    size_t bit_count;
    size_t capacity;
} BitWriter;

static int same_rle_pair(RLEPair left, RLEPair right)
{
    return left.zero_count == right.zero_count && left.value == right.value;
}

static int append_bit(BitWriter *writer, int bit)
{
    size_t byte_index = writer->bit_count / 8U;

    if (byte_index >= writer->capacity) {
        size_t new_capacity = writer->capacity == 0U ? 16U :
                              writer->capacity * 2U;
        uint8_t *new_data;

        if (new_capacity <= writer->capacity) {
            errno = EOVERFLOW;
            return 0;
        }
        new_data = realloc(writer->data, new_capacity);
        if (new_data == NULL) {
            errno = ENOMEM;
            return 0;
        }
        memset(new_data + writer->capacity, 0,
               new_capacity - writer->capacity);
        writer->data = new_data;
        writer->capacity = new_capacity;
    }

    if (bit != 0) {
        writer->data[byte_index] |=
            (uint8_t)(1U << (7U - writer->bit_count % 8U));
    }
    ++writer->bit_count;
    return 1;
}

void free_huffman_bitstream(HuffmanBitstream *bitstream)
{
    if (bitstream == NULL) {
        return;
    }
    free(bitstream->symbols);
    free(bitstream->frequencies);
    free(bitstream->data);
    memset(bitstream, 0, sizeof *bitstream);
}

int huffman_encode(const RLEPair *input, size_t input_count,
                   HuffmanBitstream *output)
{
    HuffmanNode *nodes = NULL;
    uint8_t *path = NULL;
    BitWriter writer = {0};
    size_t unique_count = 0U;
    size_t node_count;
    size_t index;
    size_t next_node;

    if (input == NULL || input_count == 0U || output == NULL ||
        input_count > UINT32_MAX || input_count > (size_t)INT_MAX / 2U ||
        input_count > SIZE_MAX / 2U) {
        errno = EINVAL;
        return 0;
    }
    memset(output, 0, sizeof *output);

    output->symbols = malloc(input_count * sizeof *output->symbols);
    output->frequencies = calloc(input_count, sizeof *output->frequencies);
    if (output->symbols == NULL || output->frequencies == NULL) {
        errno = ENOMEM;
        goto failure;
    }

    for (index = 0U; index < input_count; ++index) {
        size_t symbol;

        for (symbol = 0U; symbol < unique_count; ++symbol) {
            if (same_rle_pair(input[index], output->symbols[symbol])) {
                break;
            }
        }
        if (symbol == unique_count) {
            output->symbols[unique_count] = input[index];
            ++unique_count;
        }
        ++output->frequencies[symbol];
    }
    output->symbol_count = unique_count;

    node_count = unique_count * 2U - 1U;
    nodes = calloc(node_count, sizeof *nodes);
    path = malloc(node_count);
    if (nodes == NULL || path == NULL) {
        errno = ENOMEM;
        goto failure;
    }

    for (index = 0U; index < unique_count; ++index) {
        nodes[index].frequency = output->frequencies[index];
        nodes[index].parent = -1;
        nodes[index].left = -1;
        nodes[index].right = -1;
        nodes[index].active = 1;
    }

    next_node = unique_count;
    while (next_node < node_count) {
        int first = -1;
        int second = -1;
        size_t candidate;

        for (candidate = 0U; candidate < next_node; ++candidate) {
            if (!nodes[candidate].active) {
                continue;
            }
            if (first < 0 ||
                nodes[candidate].frequency < nodes[first].frequency ||
                (nodes[candidate].frequency == nodes[first].frequency &&
                 candidate < (size_t)first)) {
                second = first;
                first = (int)candidate;
            } else if (second < 0 ||
                       nodes[candidate].frequency < nodes[second].frequency ||
                       (nodes[candidate].frequency == nodes[second].frequency &&
                        candidate < (size_t)second)) {
                second = (int)candidate;
            }
        }

        nodes[first].active = 0;
        nodes[second].active = 0;
        nodes[first].parent = (int)next_node;
        nodes[second].parent = (int)next_node;
        nodes[next_node].frequency =
            nodes[first].frequency + nodes[second].frequency;
        nodes[next_node].parent = -1;
        nodes[next_node].left = first;
        nodes[next_node].right = second;
        nodes[next_node].active = 1;
        ++next_node;
    }

    for (index = 0U; index < input_count; ++index) {
        size_t symbol;
        size_t path_length = 0U;
        int node;

        for (symbol = 0U; symbol < unique_count; ++symbol) {
            if (same_rle_pair(input[index], output->symbols[symbol])) {
                break;
            }
        }

        node = (int)symbol;
        while (nodes[node].parent >= 0) {
            int parent = nodes[node].parent;

            path[path_length++] = (uint8_t)(nodes[parent].right == node);
            node = parent;
        }
        if (path_length == 0U) {
            path[path_length++] = 0U;
        }
        while (path_length > 0U) {
            if (!append_bit(&writer, path[--path_length])) {
                goto failure;
            }
        }
    }

    output->data = writer.data;
    output->bit_count = writer.bit_count;
    free(nodes);
    free(path);
    return 1;

failure:
    free(nodes);
    free(path);
    free(writer.data);
    free_huffman_bitstream(output);
    return 0;
}

static int write_u32(FILE *output, uint32_t value)
{
    uint8_t bytes[4] = {
        (uint8_t)(value >> 24),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),
        (uint8_t)value
    };

    return fwrite(bytes, 1U, sizeof bytes, output) == sizeof bytes;
}

static int write_u64(FILE *output, uint64_t value)
{
    uint8_t bytes[8] = {
        (uint8_t)(value >> 56),
        (uint8_t)(value >> 48),
        (uint8_t)(value >> 40),
        (uint8_t)(value >> 32),
        (uint8_t)(value >> 24),
        (uint8_t)(value >> 16),
        (uint8_t)(value >> 8),
        (uint8_t)value
    };

    return fwrite(bytes, 1U, sizeof bytes, output) == sizeof bytes;
}

int write_bitstream(FILE *output, int frame_number, uint32_t block_count,
                    const HuffmanBitstream *bitstream)
{
    static const uint8_t magic[4] = {'F', 'R', 'M', '1'};
    size_t index;
    size_t byte_count;

    if (output == NULL || bitstream == NULL || bitstream->symbols == NULL ||
        bitstream->frequencies == NULL || bitstream->data == NULL ||
        bitstream->symbol_count > UINT32_MAX) {
        errno = EINVAL;
        return 0;
    }

    byte_count = (bitstream->bit_count + 7U) / 8U;
    if (fwrite(magic, 1U, sizeof magic, output) != sizeof magic ||
        !write_u32(output, (uint32_t)(int32_t)frame_number) ||
        !write_u32(output, block_count) ||
        !write_u32(output, (uint32_t)bitstream->symbol_count) ||
        !write_u64(output, (uint64_t)bitstream->bit_count)) {
        return 0;
    }

    for (index = 0U; index < bitstream->symbol_count; ++index) {
        if (fputc(bitstream->symbols[index].zero_count, output) == EOF ||
            !write_u32(output,
                       (uint32_t)bitstream->symbols[index].value) ||
            !write_u32(output, bitstream->frequencies[index])) {
            return 0;
        }
    }

    return fwrite(bitstream->data, 1U, byte_count, output) == byte_count;
}

void reconstruct_block(Frame *frame, int block_row, int block_col,
                       Block predicted, Block residual)
{
    int quantized[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
    double coefficients[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
    double spatial_residual[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
    uint8_t *recon;
    int start_row;
    int start_column;
    int row;
    int column;

    if (frame == NULL || block_row < 0 || block_col < 0 ||
        frame->height < DCT_BLOCK_SIZE || frame->width < DCT_BLOCK_SIZE ||
        block_row >= frame->height / DCT_BLOCK_SIZE ||
        block_col >= frame->width / DCT_BLOCK_SIZE) {
        return;
    }

    recon = frame_plane(frame, FRAME_BUFFER_RECON, FRAME_PLANE_Y);
    if (recon == NULL) {
        return;
    }

    for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
        for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
            double value = residual.values[row][column];

            if (value > INT_MAX) {
                quantized[row][column] = INT_MAX;
            } else if (value < INT_MIN) {
                quantized[row][column] = INT_MIN;
            } else {
                quantized[row][column] = (int)lround(value);
            }
        }
    }
    dequantize_block(
        (const int (*)[DCT_BLOCK_SIZE])quantized, coefficients);
    inverse_dct_transform(
        (const double (*)[DCT_BLOCK_SIZE])coefficients, spatial_residual);

    start_row = block_row * DCT_BLOCK_SIZE;
    start_column = block_col * DCT_BLOCK_SIZE;
    for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
        for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
            long pixel = lround(predicted.values[row][column] +
                                spatial_residual[row][column]);

            if (pixel < 0) {
                pixel = 0;
            } else if (pixel > 255) {
                pixel = 255;
            }
            recon[(start_row + row) * frame->width +
                  start_column + column] = (uint8_t)pixel;
        }
    }
}
