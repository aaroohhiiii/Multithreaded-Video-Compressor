#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    FILE *original;
    FILE *decoded;
    uint64_t squared_error = 0U;
    size_t sample_count = 0U;
    int original_sample;
    int decoded_sample;
    double mse;
    double psnr;

    if (argc != 3) {
        return EXIT_FAILURE;
    }
    original = fopen(argv[1], "rb");
    decoded = fopen(argv[2], "rb");
    if (original == NULL || decoded == NULL) {
        if (original != NULL) {
            fclose(original);
        }
        if (decoded != NULL) {
            fclose(decoded);
        }
        return EXIT_FAILURE;
    }

    while ((original_sample = fgetc(original)) != EOF) {
        int difference;

        decoded_sample = fgetc(decoded);
        if (decoded_sample == EOF) {
            fclose(original);
            fclose(decoded);
            return EXIT_FAILURE;
        }
        difference = original_sample - decoded_sample;
        squared_error += (uint64_t)(difference * difference);
        ++sample_count;
    }
    if (fgetc(decoded) != EOF || sample_count == 0U) {
        fclose(original);
        fclose(decoded);
        return EXIT_FAILURE;
    }
    fclose(original);
    fclose(decoded);

    mse = (double)squared_error / sample_count;
    psnr = mse == 0.0 ? INFINITY : 10.0 * log10(255.0 * 255.0 / mse);
    printf("round-trip PSNR: %.2f dB (MSE %.4f)\n", psnr, mse);
    return psnr >= 30.0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
