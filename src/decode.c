#include "decode.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct DecodeNode {
    uint64_t frequency;
    int left;
    int right;
    int active;
} DecodeNode;

static int build_huffman_tree(const uint32_t *frequencies,
                              size_t symbol_count, DecodeNode **tree)
{
    size_t node_count = symbol_count * 2U - 1U;
    DecodeNode *nodes;
    size_t index;
    size_t next_node;

    nodes = calloc(node_count, sizeof *nodes);
    if (nodes == NULL) {
        errno = ENOMEM;
        return 0;
    }
    for (index = 0U; index < symbol_count; ++index) {
        nodes[index].frequency = frequencies[index];
        nodes[index].left = -1;
        nodes[index].right = -1;
        nodes[index].active = 1;
    }

    next_node = symbol_count;
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
        nodes[next_node].frequency =
            nodes[first].frequency + nodes[second].frequency;
        nodes[next_node].left = first;
        nodes[next_node].right = second;
        nodes[next_node].active = 1;
        ++next_node;
    }

    *tree = nodes;
    return 1;
}

int huffman_decode(const HuffmanBitstream *input, RLEPair **output,
                   size_t *output_count)
{
    DecodeNode *tree = NULL;
    RLEPair *decoded = NULL;
    uint64_t total_symbols = 0U;
    size_t node_count;
    size_t symbol;
    size_t produced = 0U;
    size_t bit_index;
    int node;

    if (input == NULL || output == NULL || output_count == NULL ||
        input->symbols == NULL || input->frequencies == NULL ||
        input->data == NULL || input->symbol_count == 0U ||
        input->symbol_count > (size_t)INT_MAX / 2U) {
        errno = EINVAL;
        return 0;
    }

    for (symbol = 0U; symbol < input->symbol_count; ++symbol) {
        if (input->frequencies[symbol] == 0U ||
            total_symbols > SIZE_MAX - input->frequencies[symbol]) {
            errno = EINVAL;
            return 0;
        }
        total_symbols += input->frequencies[symbol];
    }
    decoded = malloc((size_t)total_symbols * sizeof *decoded);
    if (decoded == NULL) {
        errno = ENOMEM;
        return 0;
    }

    if (!build_huffman_tree(input->frequencies, input->symbol_count, &tree)) {
        free(decoded);
        return 0;
    }
    node_count = input->symbol_count * 2U - 1U;

    if (input->symbol_count == 1U) {
        if (input->bit_count != (size_t)total_symbols) {
            errno = EINVAL;
            goto failure;
        }
        for (produced = 0U; produced < (size_t)total_symbols; ++produced) {
            decoded[produced] = input->symbols[0];
        }
    } else {
        node = (int)(node_count - 1U);
        for (bit_index = 0U;
             bit_index < input->bit_count && produced < (size_t)total_symbols;
             ++bit_index) {
            int bit = (input->data[bit_index / 8U] >>
                       (7U - bit_index % 8U)) & 1U;

            node = bit == 0 ? tree[node].left : tree[node].right;
            if (node < 0) {
                errno = EINVAL;
                goto failure;
            }
            if ((size_t)node < input->symbol_count) {
                decoded[produced++] = input->symbols[node];
                node = (int)(node_count - 1U);
            }
        }
        if (produced != (size_t)total_symbols ||
            node != (int)(node_count - 1U) || bit_index != input->bit_count) {
            errno = EINVAL;
            goto failure;
        }
    }

    free(tree);
    *output = decoded;
    *output_count = (size_t)total_symbols;
    return 1;

failure:
    free(tree);
    free(decoded);
    return 0;
}

int rle_decode(const RLEPair *input, size_t input_count,
               size_t *pairs_consumed,
               int output[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE])
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
    size_t consumed = 0U;
    size_t position = 0U;

    if (input == NULL || pairs_consumed == NULL || output == NULL) {
        errno = EINVAL;
        return 0;
    }
    memset(output, 0, sizeof(int) * DCT_BLOCK_SIZE * DCT_BLOCK_SIZE);

    while (position < DCT_BLOCK_SIZE * DCT_BLOCK_SIZE) {
        RLEPair pair;
        int coefficient_position;

        if (consumed >= input_count) {
            errno = EINVAL;
            return 0;
        }
        pair = input[consumed++];
        if (pair.zero_count > DCT_BLOCK_SIZE * DCT_BLOCK_SIZE - position) {
            errno = EINVAL;
            return 0;
        }
        position += pair.zero_count;

        if (pair.value == 0) {
            if (position != DCT_BLOCK_SIZE * DCT_BLOCK_SIZE) {
                errno = EINVAL;
                return 0;
            }
            break;
        }
        if (position >= DCT_BLOCK_SIZE * DCT_BLOCK_SIZE) {
            errno = EINVAL;
            return 0;
        }

        coefficient_position = zigzag[position];
        output[coefficient_position / DCT_BLOCK_SIZE]
              [coefficient_position % DCT_BLOCK_SIZE] = pair.value;
        ++position;
    }

    *pairs_consumed = consumed;
    return 1;
}

static int read_u32(FILE *input, uint32_t *value)
{
    uint8_t bytes[4];

    if (fread(bytes, 1U, sizeof bytes, input) != sizeof bytes) {
        return 0;
    }
    *value = (uint32_t)bytes[0] << 24 |
             (uint32_t)bytes[1] << 16 |
             (uint32_t)bytes[2] << 8 |
             bytes[3];
    return 1;
}

static int read_u64(FILE *input, uint64_t *value)
{
    uint8_t bytes[8];

    if (fread(bytes, 1U, sizeof bytes, input) != sizeof bytes) {
        return 0;
    }
    *value = (uint64_t)bytes[0] << 56 |
             (uint64_t)bytes[1] << 48 |
             (uint64_t)bytes[2] << 40 |
             (uint64_t)bytes[3] << 32 |
             (uint64_t)bytes[4] << 24 |
             (uint64_t)bytes[5] << 16 |
             (uint64_t)bytes[6] << 8 |
             bytes[7];
    return 1;
}

static int read_frame_record(FILE *input, int *frame_number,
                             uint32_t *block_count,
                             HuffmanBitstream *bitstream)
{
    uint8_t magic[4];
    uint32_t raw_frame_number;
    uint32_t symbol_count;
    uint64_t bit_count;
    size_t byte_count;
    size_t index;
    size_t magic_bytes;

    magic_bytes = fread(magic, 1U, sizeof magic, input);
    if (magic_bytes == 0U && feof(input)) {
        return 0;
    }
    if (magic_bytes != sizeof magic || memcmp(magic, "FRM1", 4U) != 0 ||
        !read_u32(input, &raw_frame_number) ||
        !read_u32(input, block_count) || !read_u32(input, &symbol_count) ||
        !read_u64(input, &bit_count) || symbol_count == 0U || bit_count == 0U ||
        symbol_count > (uint32_t)INT_MAX / 2U || bit_count > SIZE_MAX - 7U) {
        errno = EINVAL;
        return -1;
    }

    memset(bitstream, 0, sizeof *bitstream);
    bitstream->symbol_count = symbol_count;
    bitstream->bit_count = (size_t)bit_count;
    bitstream->symbols = malloc(symbol_count * sizeof *bitstream->symbols);
    bitstream->frequencies =
        malloc(symbol_count * sizeof *bitstream->frequencies);
    byte_count = (bitstream->bit_count + 7U) / 8U;
    bitstream->data = malloc(byte_count);
    if (bitstream->symbols == NULL || bitstream->frequencies == NULL ||
        bitstream->data == NULL) {
        errno = ENOMEM;
        free_huffman_bitstream(bitstream);
        return -1;
    }

    for (index = 0U; index < symbol_count; ++index) {
        int zero_count = fgetc(input);
        uint32_t value;

        if (zero_count == EOF || !read_u32(input, &value) ||
            !read_u32(input, &bitstream->frequencies[index])) {
            errno = EINVAL;
            free_huffman_bitstream(bitstream);
            return -1;
        }
        bitstream->symbols[index].zero_count = (uint8_t)zero_count;
        bitstream->symbols[index].value = (int32_t)value;
    }
    if (fread(bitstream->data, 1U, byte_count, input) != byte_count) {
        errno = EINVAL;
        free_huffman_bitstream(bitstream);
        return -1;
    }

    *frame_number = (int32_t)raw_frame_number;
    return 1;
}

static int decode_plane(Frame *frame, RLEPair *pairs, size_t pair_count,
                        size_t *pair_index, uint32_t *blocks_decoded)
{
    int block_row;
    int block_column;

    for (block_row = 0; block_row < frame->height / DCT_BLOCK_SIZE;
         ++block_row) {
        for (block_column = 0;
             block_column < frame->width / DCT_BLOCK_SIZE;
             ++block_column) {
            int quantized[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
            double coefficients[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
            double spatial_residual[DCT_BLOCK_SIZE][DCT_BLOCK_SIZE];
            uint8_t *recon;
            size_t consumed;
            int prediction;
            int row;
            int column;

            if (!rle_decode(pairs + *pair_index, pair_count - *pair_index,
                            &consumed, quantized)) {
                return 0;
            }
            *pair_index += consumed;
            dequantize_block(
                (const int (*)[DCT_BLOCK_SIZE])quantized, coefficients);
            inverse_dct_transform(
                (const double (*)[DCT_BLOCK_SIZE])coefficients,
                spatial_residual);

            prediction = predict_from_neighbors(frame, block_row, block_column);
            recon = frame_plane(frame, FRAME_BUFFER_RECON, FRAME_PLANE_Y);
            if (recon == NULL) {
                errno = EINVAL;
                return 0;
            }
            for (row = 0; row < DCT_BLOCK_SIZE; ++row) {
                for (column = 0; column < DCT_BLOCK_SIZE; ++column) {
                    int pixel = (int)lround(
                        prediction + spatial_residual[row][column]);
                    int frame_row = block_row * DCT_BLOCK_SIZE + row;
                    int frame_column =
                        block_column * DCT_BLOCK_SIZE + column;

                    if (pixel < 0) {
                        pixel = 0;
                    } else if (pixel > 255) {
                        pixel = 255;
                    }
                    recon[frame_row * frame->width + frame_column] =
                        (uint8_t)pixel;
                }
            }
            ++*blocks_decoded;
        }
    }
    return 1;
}

int decode_stream(FILE *input, FILE *raw_output, int width, int height)
{
    Frame decoded = {.width = width, .height = height};
    size_t luma_size;
    size_t frame_size;
    size_t expected_block_count;
    uint32_t expected_blocks;
    int frames_decoded = 0;

    if (input == NULL || raw_output == NULL || width <= 0 || height <= 0 ||
        width % 16 != 0 || height % 16 != 0 ||
        (size_t)width > SIZE_MAX / (size_t)height) {
        errno = EINVAL;
        return -1;
    }
    luma_size = (size_t)width * (size_t)height;
    if (luma_size > SIZE_MAX - luma_size / 2U) {
        errno = EOVERFLOW;
        return -1;
    }
    frame_size = luma_size + luma_size / 2U;
    expected_block_count =
        (size_t)(width / DCT_BLOCK_SIZE) *
        (size_t)(height / DCT_BLOCK_SIZE) +
        2U * (size_t)(width / (2 * DCT_BLOCK_SIZE)) *
        (size_t)(height / (2 * DCT_BLOCK_SIZE));
    if (expected_block_count > UINT32_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    expected_blocks = (uint32_t)expected_block_count;
    decoded.recon = malloc(frame_size);
    if (decoded.recon == NULL) {
        errno = ENOMEM;
        return -1;
    }

    for (;;) {
        HuffmanBitstream bitstream = {0};
        RLEPair *pairs = NULL;
        size_t pair_count = 0U;
        size_t pair_index = 0U;
        uint32_t encoded_blocks;
        uint32_t blocks_decoded = 0U;
        Frame plane;
        int record_result;

        record_result = read_frame_record(input, &decoded.frame_number,
                                          &encoded_blocks, &bitstream);
        if (record_result == 0) {
            break;
        }
        if (record_result < 0 || encoded_blocks != expected_blocks) {
            errno = EINVAL;
            free_huffman_bitstream(&bitstream);
            frames_decoded = -1;
            break;
        }
        memset(decoded.recon, 0, frame_size);

        if (!huffman_decode(&bitstream, &pairs, &pair_count)) {
            free_huffman_bitstream(&bitstream);
            frames_decoded = -1;
            break;
        }
        free_huffman_bitstream(&bitstream);

        plane.frame_number = decoded.frame_number;
        plane.width = width;
        plane.height = height;
        plane.orig = NULL;
        plane.recon = frame_plane(&decoded, FRAME_BUFFER_RECON, FRAME_PLANE_Y);
        if (!decode_plane(&plane, pairs, pair_count, &pair_index,
                          &blocks_decoded)) {
            free(pairs);
            frames_decoded = -1;
            break;
        }

        plane.width = width / 2;
        plane.height = height / 2;
        plane.recon = frame_plane(&decoded, FRAME_BUFFER_RECON, FRAME_PLANE_U);
        if (!decode_plane(&plane, pairs, pair_count, &pair_index,
                          &blocks_decoded)) {
            free(pairs);
            frames_decoded = -1;
            break;
        }
        plane.recon = frame_plane(&decoded, FRAME_BUFFER_RECON, FRAME_PLANE_V);
        if (!decode_plane(&plane, pairs, pair_count, &pair_index,
                          &blocks_decoded) || pair_index != pair_count ||
            blocks_decoded != encoded_blocks) {
            free(pairs);
            errno = EINVAL;
            frames_decoded = -1;
            break;
        }
        free(pairs);

        if (fwrite(decoded.recon, 1U, frame_size, raw_output) != frame_size) {
            frames_decoded = -1;
            break;
        }
        ++frames_decoded;
    }

    free(decoded.recon);
    return frames_decoded;
}
