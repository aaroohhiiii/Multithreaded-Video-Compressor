#include "decode.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

static int parse_dimension(const char *text, int *value)
{
    char *end;
    long parsed;

    errno = 0;
    parsed = strtol(text, &end, 10);
    if (errno != 0 || *text == '\0' || *end != '\0' || parsed <= 0 ||
        parsed > INT_MAX || parsed % 16 != 0) {
        return 0;
    }
    *value = (int)parsed;
    return 1;
}

int main(int argc, char **argv)
{
    FILE *input;
    FILE *output;
    int width;
    int height;
    int frame_count;

    if (argc != 5 || !parse_dimension(argv[3], &width) ||
        !parse_dimension(argv[4], &height)) {
        fprintf(stderr,
                "usage: %s input.bin output.yuv width height\n"
                "width and height must be positive multiples of 16\n",
                argv[0]);
        return EXIT_FAILURE;
    }

    input = fopen(argv[1], "rb");
    if (input == NULL) {
        perror(argv[1]);
        return EXIT_FAILURE;
    }
    output = fopen(argv[2], "wb");
    if (output == NULL) {
        perror(argv[2]);
        fclose(input);
        return EXIT_FAILURE;
    }

    frame_count = decode_stream(input, output, width, height);
    if (frame_count < 0) {
        perror("decoding stream");
    } else {
        printf("decoded %d frame%s\n", frame_count,
               frame_count == 1 ? "" : "s");
    }

    if (fclose(input) != 0 && frame_count >= 0) {
        frame_count = -1;
    }
    if (fclose(output) != 0 && frame_count >= 0) {
        frame_count = -1;
    }
    return frame_count < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
