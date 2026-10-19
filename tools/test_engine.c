#include "EarCueAudio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int find_device(const HADeviceInfo *devices, int count, const char *needle) {
    for (int i = 0; i < count; i++) {
        if (strstr(devices[i].name, needle)) return i;
    }
    return -1;
}

int main(int argc, char **argv) {
    HADeviceInfo devices[32];
    int count = ha_list_devices(devices, 32);
    printf("devices: %d\n", count);
    for (int i = 0; i < count; i++) {
        printf("  [%u] %s in=%d out=%d sr=%.0f uid=%s\n",
               devices[i].id, devices[i].name,
               devices[i].input_channels, devices[i].output_channels,
               devices[i].sample_rate, devices[i].uid);
    }
    printf("default input=%u output=%u\n",
           ha_default_input_device(), ha_default_output_device());

    char json[65536];
    int json_len = ha_list_devices_json(json, (int)sizeof(json));
    printf("json (%d): %s\n", json_len, json);

    const char *input_name = argc > 1 ? argv[1] : "外置麦克风";
    const char *output_name = argc > 2 ? argv[2] : "Oray";
    double seconds = argc > 3 ? atof(argv[3]) : 2.0;
    int mic = find_device(devices, count, input_name);
    int out = find_device(devices, count, output_name);
    if (mic < 0 || out < 0) {
        printf("missing devices: %s=%d %s=%d\n", input_name, mic, output_name, out);
        return 1;
    }
    printf("\nstarting: %s -> %s for %.1fs\n", devices[mic].name, devices[out].name, seconds);
    HAEngine *engine = ha_engine_create(devices[mic].id, devices[out].id, 32, 48000.0);
    if (!engine) {
        printf("engine alloc failed\n");
        return 1;
    }
    printf("create error: '%s'\n", ha_engine_error(engine));
    printf("latency frames: %d, sr: %.0f\n",
           ha_engine_latency_frames(engine), ha_engine_sample_rate(engine));
    ha_engine_set_reverb(engine, 2, 0.25f);
    ha_engine_set_gain(engine, 8.0f, 0.5f, 0);
    ha_engine_set_cleanup(engine, 1, 1, -45.0f);
    ha_engine_set_eq(engine, 1, -2.0f, 2.0f, 1.0f);
    ha_engine_set_compressor(engine, 1, -30.0f, 3.0f);
    ha_engine_set_feedback_suppression(engine, 1, 1);
    if (ha_engine_start(engine) != 0) {
        printf("start error: '%s'\n", ha_engine_error(engine));
        ha_engine_destroy(engine);
        return 1;
    }
    printf("running=%d\n", ha_engine_is_running(engine));
    int steps = (int)(seconds / 0.25);
    if (steps < 4) steps = 4;
    for (int i = 0; i < steps; i++) {
        if (i == steps / 2) {
            ha_engine_set_gain(engine, 1.5f, 0.8f, 1);
            printf("  -- muted --\n");
        } else if (i == steps / 2 + 1) {
            ha_engine_set_gain(engine, 1.5f, 0.8f, 0);
            printf("  -- unmuted --\n");
        }
        usleep(250000);
        printf("  in_rms=%.5f in_peak=%.5f out_rms=%.5f out_peak=%.5f clips=%d overloads=%d callbacks=%llu\n",
               ha_engine_input_level(engine),
               ha_engine_input_peak(engine),
               ha_engine_output_level(engine),
               ha_engine_output_peak(engine),
               ha_engine_clip_count(engine),
               ha_engine_overload_count(engine),
               ha_engine_callback_count(engine));
    }
    ha_engine_stop(engine);
    printf("stopped\n");
    ha_engine_destroy(engine);
    return 0;
}
