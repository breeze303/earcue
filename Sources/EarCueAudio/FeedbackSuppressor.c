#include "FeedbackSuppressor.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define HS_ANALYSIS_SIZE 1024
#define HS_BANDS 48
#define HS_NOTCHES 4
#define HS_PI 3.14159265358979323846f
#define HS_NOTCH_HOLD 100

typedef struct {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
    float z1;
    float z2;
    float freq;
    int active;
    int hold;
} HNotch;

struct HFeedbackSuppressor {
    int sample_rate;
    int enabled;
    int sensitivity;
    int analysis_pos;
    float band_ratio;
    float analysis[HS_ANALYSIS_SIZE];
    float window[HS_ANALYSIS_SIZE];
    float goertzel_coeff[HS_BANDS];
    float goertzel_freq[HS_BANDS];
    float power[HS_BANDS];
    float candidate[HS_BANDS];
    HNotch notches[HS_NOTCHES];
    int notch_count;
};

static float clamp_freq(float freq, int sample_rate) {
    float max_freq = (float)sample_rate * 0.45f;
    if (freq < 80.0f) return 80.0f;
    if (freq > max_freq) return max_freq;
    return freq;
}

static void notch_design(HNotch *notch, int sample_rate, float freq) {
    freq = clamp_freq(freq, sample_rate);
    float w0 = 2.0f * HS_PI * freq / (float)sample_rate;
    float cw = cosf(w0);
    float sw = sinf(w0);
    float alpha = sw / (2.0f * 25.0f);
    float a0 = 1.0f + alpha;

    notch->b0 = 1.0f / a0;
    notch->b1 = -2.0f * cw / a0;
    notch->b2 = 1.0f / a0;
    notch->a1 = -2.0f * cw / a0;
    notch->a2 = (1.0f - alpha) / a0;
    notch->z1 = 0.0f;
    notch->z2 = 0.0f;
    notch->freq = freq;
    notch->active = 1;
    notch->hold = HS_NOTCH_HOLD;
}

static inline float notch_process(HNotch *notch, float x) {
    float y = notch->b0 * x + notch->z1;
    notch->z1 = notch->b1 * x - notch->a1 * y + notch->z2;
    notch->z2 = notch->b2 * x - notch->a2 * y;
    return y;
}

HFeedbackSuppressor *hs_create(int sample_rate) {
    if (sample_rate <= 0) return NULL;
    HFeedbackSuppressor *hs = (HFeedbackSuppressor *)calloc(1, sizeof(*hs));
    if (!hs) return NULL;

    hs->sample_rate = sample_rate;
    hs->sensitivity = 1;
    hs->band_ratio = powf(8000.0f / 150.0f, 1.0f / (float)(HS_BANDS - 1));

    for (int k = 0; k < HS_BANDS; k++) {
        float freq = 150.0f * powf(hs->band_ratio, (float)k);
        hs->goertzel_freq[k] = freq;
        hs->goertzel_coeff[k] = 2.0f * cosf(2.0f * HS_PI * freq / (float)sample_rate);
    }
    for (int n = 0; n < HS_ANALYSIS_SIZE; n++) {
        hs->window[n] = 0.5f - 0.5f * cosf(2.0f * HS_PI * (float)n /
                                            (float)(HS_ANALYSIS_SIZE - 1));
    }
    return hs;
}

void hs_destroy(HFeedbackSuppressor *hs) {
    free(hs);
}

void hs_set_enabled(HFeedbackSuppressor *hs, int enabled, int sensitivity) {
    if (!hs) return;
    if (sensitivity < 0) sensitivity = 0;
    if (sensitivity > 2) sensitivity = 2;
    if (enabled && !hs->enabled) {
        hs->analysis_pos = 0;
        memset(hs->candidate, 0, sizeof(hs->candidate));
        for (int i = 0; i < HS_NOTCHES; i++) {
            hs->notches[i].active = 0;
        }
        hs->notch_count = 0;
    }
    hs->enabled = enabled ? 1 : 0;
    hs->sensitivity = sensitivity;
}

int hs_notch_count(HFeedbackSuppressor *hs) {
    return hs ? hs->notch_count : 0;
}

static void hs_confirm_notch(HFeedbackSuppressor *hs, float freq) {
    // Reuse an existing notch if it is already close in log-frequency.
    for (int i = 0; i < HS_NOTCHES; i++) {
        HNotch *notch = &hs->notches[i];
        if (!notch->active) continue;
        float ratio = freq / notch->freq;
        if (ratio > 0.92f && ratio < 1.08f) {
            notch->hold = HS_NOTCH_HOLD;
            return;
        }
    }

    // Pick an inactive slot, otherwise the one with the shortest hold.
    HNotch *slot = NULL;
    for (int i = 0; i < HS_NOTCHES; i++) {
        if (!hs->notches[i].active) {
            slot = &hs->notches[i];
            break;
        }
    }
    if (!slot) {
        slot = &hs->notches[0];
        for (int i = 1; i < HS_NOTCHES; i++) {
            if (hs->notches[i].hold < slot->hold) slot = &hs->notches[i];
        }
    }
    notch_design(slot, hs->sample_rate, freq);
}

static float hs_refine_frequency(HFeedbackSuppressor *hs, int band) {
    int low_index = band > 0 ? band - 1 : band;
    int high_index = band < HS_BANDS - 1 ? band + 1 : band;
    float low = hs->goertzel_freq[low_index];
    float high = hs->goertzel_freq[high_index];
    if (high <= low) return hs->goertzel_freq[band];

    const int steps = 32;
    float powers[33];
    int best_step = 0;
    float best_power = -1.0f;
    float fs = (float)hs->sample_rate;

    for (int step = 0; step <= steps; step++) {
        float freq = low + (high - low) * (float)step / (float)steps;
        float coeff = 2.0f * cosf(2.0f * HS_PI * freq / fs);
        float s0 = 0.0f;
        float s1 = 0.0f;
        float s2 = 0.0f;
        for (int n = 0; n < HS_ANALYSIS_SIZE; n++) {
            float x = hs->analysis[n] * hs->window[n];
            s0 = x + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        float p = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        powers[step] = p;
        if (p > best_power) {
            best_power = p;
            best_step = step;
        }
    }

    float best_freq = low + (high - low) * (float)best_step / (float)steps;
    if (best_step > 0 && best_step < steps) {
        float denom = powers[best_step - 1] - 2.0f * powers[best_step] +
                      powers[best_step + 1];
        if (fabsf(denom) > 1e-12f) {
            float delta = 0.5f * (powers[best_step - 1] - powers[best_step + 1]) / denom;
            if (delta > -1.0f && delta < 1.0f) {
                best_freq += delta * (high - low) / (float)steps;
            }
        }
    }
    return best_freq;
}

static void hs_analyze(HFeedbackSuppressor *hs) {
    const float ratio = hs->sensitivity == 0 ? 30.0f
                      : hs->sensitivity == 1 ? 15.0f
                      : 8.0f;
    const float absolute_floor = 1e-4f;

    for (int k = 0; k < HS_BANDS; k++) {
        float s0 = 0.0f;
        float s1 = 0.0f;
        float s2 = 0.0f;
        const float coeff = hs->goertzel_coeff[k];
        for (int n = 0; n < HS_ANALYSIS_SIZE; n++) {
            float x = hs->analysis[n] * hs->window[n];
            s0 = x + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        float p = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        hs->power[k] = p / (0.25f * (float)HS_ANALYSIS_SIZE *
                            (float)HS_ANALYSIS_SIZE);
    }

    for (int k = 0; k < HS_BANDS; k++) {
        int left_index = k > 0 ? k - 1 : k;
        int right_index = k < HS_BANDS - 1 ? k + 1 : k;
        int left2_index = k > 1 ? k - 2 : 0;
        int right2_index = k < HS_BANDS - 2 ? k + 2 : HS_BANDS - 1;
        int left3_index = k > 2 ? k - 3 : 0;
        int right3_index = k < HS_BANDS - 3 ? k + 3 : HS_BANDS - 1;

        /* Exclude the immediate neighbours from the baseline: a howl
           straddling two Goertzel bands would otherwise raise the
           baseline enough to hide itself. */
        float baseline = (hs->power[left2_index] + hs->power[right2_index] +
                          hs->power[left3_index] + hs->power[right3_index]) * 0.25f;
        float p = hs->power[k];

        int is_peak = p > absolute_floor &&
                      p > baseline * ratio &&
                      p >= hs->power[left_index] &&
                      p >= hs->power[right_index];
        if (is_peak) {
            hs->candidate[k] += 1.0f;
        } else {
            hs->candidate[k] *= 0.8f;
        }

        if (hs->candidate[k] >= 2.0f) {
            /* Refine the frequency with a short fine-grained Goertzel scan
               between the neighbouring bands before creating the notch. */
            float freq = hs_refine_frequency(hs, k);
            hs_confirm_notch(hs, freq);
            hs->candidate[k] = 0.0f;
        }
    }

    int active = 0;
    for (int i = 0; i < HS_NOTCHES; i++) {
        HNotch *notch = &hs->notches[i];
        if (!notch->active) continue;
        notch->hold -= 1;
        if (notch->hold <= 0) {
            notch->active = 0;
        } else {
            active++;
        }
    }
    hs->notch_count = active;
}

float hs_process(HFeedbackSuppressor *hs, float sample) {
    if (!hs || !hs->enabled) return sample;

    float input = sample;
    for (int i = 0; i < HS_NOTCHES; i++) {
        if (hs->notches[i].active) {
            sample = notch_process(&hs->notches[i], sample);
        }
    }

    hs->analysis[hs->analysis_pos++] = input;
    if (hs->analysis_pos >= HS_ANALYSIS_SIZE) {
        hs->analysis_pos = 0;
        hs_analyze(hs);
    }
    return sample;
}
