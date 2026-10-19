#include "FeedbackSuppressor.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

int main(void) {
    const int sr = 48000;
    const int n = sr * 3;
    HFeedbackSuppressor *hs = hs_create(sr);
    if (!hs) { printf("create failed\n"); return 1; }
    hs_set_enabled(hs, 1, 1);

    double input_energy = 0.0;
    double output_energy = 0.0;
    float block[32];
    int block_index = 0;
    unsigned int seed = 12345;
    float peak_out = 0.0f;

    for (int i = 0; i < n; i++) {
        float tone = 0.3f * sinf(2.0f * 3.14159265f * 1000.0f * (float)i / (float)sr);
        seed = seed * 1664525u + 1013904223u;
        float noise = ((float)((seed >> 8) & 0xFFFF) / 32768.0f - 1.0f) * 0.01f;
        float input = tone + noise;
        if (i > sr / 2) input_energy += (double)input * input;
        block[block_index++] = input;
        if (block_index == 32) {
            for (int j = 0; j < 32; j++) {
                float out = hs_process(hs, block[j]);
                if (i > sr / 2) output_energy += (double)out * out;
                float a = fabsf(out);
                if (a > peak_out) peak_out = a;
            }
            block_index = 0;
            if ((i / 32) % 375 == 374) {
                printf("t=%.2fs notches=%d peak=%.3f\n",
                       (double)i / sr, hs_notch_count(hs), peak_out);
            }
        }
    }

    printf("input_rms=%.4f output_rms=%.4f notches=%d peak=%.3f\n",
           sqrt(input_energy / n), sqrt(output_energy / n),
           hs_notch_count(hs), peak_out);
    hs_destroy(hs);
    return 0;
}
