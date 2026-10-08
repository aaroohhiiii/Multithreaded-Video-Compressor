#include "codec.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

int main(void)
{
    static const uint8_t known_frame[] = {
        0, 1, 2, 3, 4, 5, 6, 7, /* 4x2 Y plane */
        100, 101,                /* 2x1 U plane */
        200, 201                 /* 2x1 V plane */
    };
    Frame frame = {.frame_number = 0, .width = 4, .height = 2};
    FILE *input = tmpfile();
    uint8_t *y;
    uint8_t *u;
    uint8_t *v;
    size_t index;

    assert(input != NULL);
    assert(fwrite(known_frame, 1U, sizeof known_frame, input) ==
           sizeof known_frame);
    rewind(input);

    assert(read_frame(input, &frame) == READ_FRAME_OK);
    y = frame_plane(&frame, FRAME_BUFFER_ORIG, FRAME_PLANE_Y);
    u = frame_plane(&frame, FRAME_BUFFER_ORIG, FRAME_PLANE_U);
    v = frame_plane(&frame, FRAME_BUFFER_ORIG, FRAME_PLANE_V);
    assert(y != NULL && u != NULL && v != NULL);

    printf("Y:");
    for (index = 0U; index < 4U; ++index) {
        printf(" %u", (unsigned)y[index]);
    }
    printf("\nU: %u %u\n", (unsigned)u[0], (unsigned)u[1]);
    printf("V: %u %u\n", (unsigned)v[0], (unsigned)v[1]);

    assert(y[0] == 0 && y[3] == 3);
    assert(u[0] == 100 && u[1] == 101);
    assert(v[0] == 200 && v[1] == 201);
    for (index = 0U; index < sizeof known_frame; ++index) {
        assert(frame.recon[index] == 0);
    }
    assert(read_frame(input, &frame) == READ_FRAME_EOF);

    free(frame.orig);
    free(frame.recon);
    fclose(input);
    return 0;
}
