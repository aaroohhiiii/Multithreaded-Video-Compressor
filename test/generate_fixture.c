#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    enum { WIDTH = 32, HEIGHT = 32 };
    FILE *output;
    int row;
    int column;

    if (argc != 2) {
        return EXIT_FAILURE;
    }
    output = fopen(argv[1], "wb");
    if (output == NULL) {
        return EXIT_FAILURE;
    }

    for (row = 0; row < HEIGHT; ++row) {
        for (column = 0; column < WIDTH; ++column) {
            uint8_t y = (uint8_t)(40 + row * 2 + column);
            if (fwrite(&y, 1U, 1U, output) != 1U) {
                fclose(output);
                return EXIT_FAILURE;
            }
        }
    }
    for (row = 0; row < HEIGHT / 2; ++row) {
        for (column = 0; column < WIDTH / 2; ++column) {
            uint8_t u = (uint8_t)(110 + column / 4);
            if (fwrite(&u, 1U, 1U, output) != 1U) {
                fclose(output);
                return EXIT_FAILURE;
            }
        }
    }
    for (row = 0; row < HEIGHT / 2; ++row) {
        for (column = 0; column < WIDTH / 2; ++column) {
            uint8_t v = (uint8_t)(140 + row / 4);
            if (fwrite(&v, 1U, 1U, output) != 1U) {
                fclose(output);
                return EXIT_FAILURE;
            }
        }
    }

    return fclose(output) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
