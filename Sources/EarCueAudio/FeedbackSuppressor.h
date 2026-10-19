#ifndef FEEDBACK_SUPPRESSOR_H
#define FEEDBACK_SUPPRESSOR_H

typedef struct HFeedbackSuppressor HFeedbackSuppressor;

HFeedbackSuppressor *hs_create(int sample_rate);
void hs_destroy(HFeedbackSuppressor *hs);

/* sensitivity: 0 = low, 1 = medium, 2 = high */
void hs_set_enabled(HFeedbackSuppressor *hs, int enabled, int sensitivity);
float hs_process(HFeedbackSuppressor *hs, float sample);
int hs_notch_count(HFeedbackSuppressor *hs);

#endif
