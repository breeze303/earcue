#ifndef EAR_CUE_AUDIO_H
#define EAR_CUE_AUDIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t id;
    char name[256];
    char uid[256];
    int input_channels;
    int output_channels;
    double sample_rate;
} HADeviceInfo;

typedef struct HAEngine HAEngine;

/* Device enumeration. Returns the number of devices written (up to max). */
int ha_list_devices(HADeviceInfo *devices, int max_devices);

/* Swift-friendly JSON array of devices. Returns bytes written, or -1. */
int ha_list_devices_json(char *buffer, int buffer_size);

uint32_t ha_default_input_device(void);
uint32_t ha_default_output_device(void);

/* Create an engine for the given input/output devices. Does not start audio. */
HAEngine *ha_engine_create(uint32_t input_device,
                           uint32_t output_device,
                           int buffer_frames,
                           double sample_rate);
void ha_engine_destroy(HAEngine *engine);

int ha_engine_start(HAEngine *engine);
void ha_engine_stop(HAEngine *engine);

/* Reverb preset: 0 = off, 1 = room, 2 = hall, 3 = large hall. */
void ha_engine_set_reverb(HAEngine *engine, int preset, float wet);

/* Live gain/mute and cleanup controls. */
void ha_engine_set_gain(HAEngine *engine, float input_gain, float output_gain, int muted);
void ha_engine_set_cleanup(HAEngine *engine, int highpass, int gate,
                           float gate_threshold_db);

/* 3-band EQ and compressor. Limiting is always on after output gain. */
void ha_engine_set_eq(HAEngine *engine, int enabled,
                      float low_db, float mid_db, float high_db);
void ha_engine_set_compressor(HAEngine *engine, int enabled,
                              float threshold_db, float ratio);

/* Adaptive feedback suppression (spectral howl detection + notches). */
void ha_engine_set_feedback_suppression(HAEngine *engine,
                                        int enabled, int sensitivity);
int ha_engine_feedback_notch_count(HAEngine *engine);

/* Meters. Levels are RMS; peaks are absolute sample peaks. */
float ha_engine_input_level(HAEngine *engine);
float ha_engine_input_peak(HAEngine *engine);
float ha_engine_output_level(HAEngine *engine);
float ha_engine_output_peak(HAEngine *engine);

/* Diagnostics. */
const char *ha_engine_error(HAEngine *engine);
int ha_engine_latency_frames(HAEngine *engine);
double ha_engine_sample_rate(HAEngine *engine);
int ha_engine_is_running(HAEngine *engine);
unsigned long long ha_engine_callback_count(HAEngine *engine);
int ha_engine_overload_count(HAEngine *engine);
void ha_engine_reset_overload_count(HAEngine *engine);
int ha_engine_clip_count(HAEngine *engine);
void ha_engine_reset_clip_count(HAEngine *engine);

#ifdef __cplusplus
}
#endif

#endif
