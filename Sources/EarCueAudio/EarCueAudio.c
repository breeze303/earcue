#include "EarCueAudio.h"
#include "FeedbackSuppressor.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/AudioHardware.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_FRAMES 4096
#define REVERB_COMBS 4
#define REVERB_ALLPASSES 2

typedef struct {
    float *buffer;
    int length;
    int pos;
    float feedback;
} HDelay;

typedef struct {
    HDelay combs[REVERB_COMBS];
    HDelay allpasses[REVERB_ALLPASSES];
    float damping;
    float damp_state;
    int sample_rate;
} HReverb;

typedef struct {
    float b0;
    float b1;
    float b2;
    float a1;
    float a2;
    float z1;
    float z2;
} HBiquad;

struct HAEngine {
    AudioDeviceID input_device;
    AudioDeviceID output_device;
    AudioDeviceID aggregate_device;
    AudioUnit unit;
    double sample_rate;
    int requested_buffer_frames;
    int actual_buffer_frames;
    int in_channels;
    int out_channels;
    int running;
    char error[512];

    AudioBufferList *input_abl;
    float *input_buffer;
    float *mix_buffer;

    HReverb reverb;
    _Atomic int reverb_preset;
    _Atomic float reverb_wet;

    _Atomic float input_gain;
    _Atomic float output_gain;
    _Atomic int muted;
    _Atomic int highpass_enabled;
    _Atomic int gate_enabled;
    _Atomic float gate_threshold;

    _Atomic float input_level;
    _Atomic float input_peak;
    _Atomic float output_level;
    _Atomic float output_peak;
    _Atomic int clip_count;
    _Atomic unsigned long long callback_count;

    AudioDeviceID overload_device;
    int overload_listener_installed;
    _Atomic int overload_count;

    float hp_x1;
    float hp_y1;
    float hp_a;
    float gate_env;
    float gate_gain;
    float gate_attack;
    float gate_release;
    float gate_env_release;

    _Atomic int eq_enabled;
    _Atomic float eq_low_db;
    _Atomic float eq_mid_db;
    _Atomic float eq_high_db;
    HBiquad eq_low;
    HBiquad eq_mid;
    HBiquad eq_high;
    float eq_last_low;
    float eq_last_mid;
    float eq_last_high;
    int eq_last_enabled;

    _Atomic int comp_enabled;
    _Atomic float comp_threshold_db;
    _Atomic float comp_ratio;
    float comp_env;
    float comp_gain;
    float comp_attack;
    float comp_release;

    HFeedbackSuppressor *feedback_suppressor;
    float gate_noise_floor;
};

static void set_error(HAEngine *e, const char *fmt, ...) {
    if (!e) return;
    va_list args;
    va_start(args, fmt);
    vsnprintf(e->error, sizeof(e->error), fmt, args);
    va_end(args);
}

static int set_error_status(HAEngine *e, OSStatus status, const char *what) {
    set_error(e, "%s failed (OSStatus %d)", what, (int)status);
    return -1;
}

static CFStringRef copy_device_uid(AudioDeviceID device) {
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal,
        0
    };
    CFStringRef uid = NULL;
    UInt32 size = sizeof(uid);
    OSStatus status = AudioObjectGetPropertyData(device, &addr, 0, NULL, &size, &uid);
    return status == noErr ? uid : NULL;
}

static int copy_device_name(AudioDeviceID device, char *out, size_t out_size) {
    AudioObjectPropertyAddress addr = {
        kAudioObjectPropertyName,
        kAudioObjectPropertyScopeGlobal,
        0
    };
    CFStringRef name = NULL;
    UInt32 size = sizeof(name);
    OSStatus status = AudioObjectGetPropertyData(device, &addr, 0, NULL, &size, &name);
    if (status != noErr || !name) return -1;
    Boolean ok = CFStringGetCString(name, out, (CFIndex)out_size, kCFStringEncodingUTF8);
    CFRelease(name);
    return ok ? 0 : -1;
}

static int get_channel_count(AudioDeviceID device, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyStreamConfiguration,
        scope,
        0
    };
    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(device, &addr, 0, NULL, &size);
    if (status != noErr || size == 0) return 0;
    AudioBufferList *list = (AudioBufferList *)malloc(size);
    if (!list) return 0;
    status = AudioObjectGetPropertyData(device, &addr, 0, NULL, &size, list);
    int channels = 0;
    if (status == noErr) {
        for (UInt32 i = 0; i < list->mNumberBuffers; i++) {
            channels += (int)list->mBuffers[i].mNumberChannels;
        }
    }
    free(list);
    return channels;
}

static double get_nominal_sample_rate(AudioDeviceID device) {
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyNominalSampleRate,
        kAudioObjectPropertyScopeGlobal,
        0
    };
    Float64 rate = 0;
    UInt32 size = sizeof(rate);
    OSStatus status = AudioObjectGetPropertyData(device, &addr, 0, NULL, &size, &rate);
    return status == noErr ? (double)rate : 0.0;
}

static UInt32 get_default_device(AudioObjectPropertySelector selector) {
    AudioObjectPropertyAddress addr = {
        selector,
        kAudioObjectPropertyScopeGlobal,
        0
    };
    AudioDeviceID device = kAudioObjectUnknown;
    UInt32 size = sizeof(device);
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &size, &device);
    return device;
}

int ha_list_devices(HADeviceInfo *devices, int max_devices) {
    if (!devices || max_devices <= 0) return 0;

    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal,
        0
    };
    UInt32 size = 0;
    OSStatus status = AudioObjectGetPropertyDataSize(
        kAudioObjectSystemObject, &addr, 0, NULL, &size);
    if (status != noErr || size == 0) return 0;

    int count = (int)(size / sizeof(AudioDeviceID));
    AudioDeviceID *ids = (AudioDeviceID *)malloc(size);
    if (!ids) return 0;
    status = AudioObjectGetPropertyData(
        kAudioObjectSystemObject, &addr, 0, NULL, &size, ids);
    if (status != noErr) {
        free(ids);
        return 0;
    }

    int written = 0;
    for (int i = 0; i < count && written < max_devices; i++) {
        AudioDeviceID device = ids[i];
        int inputs = get_channel_count(device, kAudioObjectPropertyScopeInput);
        int outputs = get_channel_count(device, kAudioObjectPropertyScopeOutput);
        if (inputs == 0 && outputs == 0) continue;

        HADeviceInfo *info = &devices[written];
        memset(info, 0, sizeof(*info));
        info->id = device;
        info->input_channels = inputs;
        info->output_channels = outputs;
        info->sample_rate = get_nominal_sample_rate(device);
        copy_device_name(device, info->name, sizeof(info->name));
        CFStringRef uid = copy_device_uid(device);
        if (uid) {
            CFStringGetCString(uid, info->uid, sizeof(info->uid), kCFStringEncodingUTF8);
            CFRelease(uid);
        }
        written++;
    }

    free(ids);
    return written;
}

uint32_t ha_default_input_device(void) {
    return (uint32_t)get_default_device(kAudioHardwarePropertyDefaultInputDevice);
}

uint32_t ha_default_output_device(void) {
    return (uint32_t)get_default_device(kAudioHardwarePropertyDefaultOutputDevice);
}

static int json_escape(char *out, int capacity, int pos, const char *text) {
    for (const unsigned char *p = (const unsigned char *)text; *p; p++) {
        if (pos >= capacity - 1) return -1;
        switch (*p) {
            case '"':
                out[pos++] = '\\'; out[pos++] = '"'; break;
            case '\\':
                out[pos++] = '\\'; out[pos++] = '\\'; break;
            case '\n':
                out[pos++] = '\\'; out[pos++] = 'n'; break;
            case '\r':
                out[pos++] = '\\'; out[pos++] = 'r'; break;
            case '\t':
                out[pos++] = '\\'; out[pos++] = 't'; break;
            default:
                if (*p < 0x20) {
                    if (pos >= capacity - 6) return -1;
                    pos += snprintf(out + pos, (size_t)(capacity - pos), "\\u%04x", *p);
                } else {
                    out[pos++] = (char)*p;
                }
        }
    }
    return pos;
}

int ha_list_devices_json(char *buffer, int buffer_size) {
    if (!buffer || buffer_size < 3) return -1;

    HADeviceInfo devices[64];
    int count = ha_list_devices(devices, 64);
    int pos = 0;
    buffer[pos++] = '[';

    for (int i = 0; i < count; i++) {
        if (i > 0) {
            if (pos >= buffer_size - 1) return -1;
            buffer[pos++] = ',';
        }
        int written = snprintf(buffer + pos, (size_t)(buffer_size - pos),
                               "{\"id\":%u,\"name\":\"", devices[i].id);
        if (written < 0 || written >= buffer_size - pos) return -1;
        pos += written;
        pos = json_escape(buffer, buffer_size, pos, devices[i].name);
        if (pos < 0) return -1;
        written = snprintf(buffer + pos, (size_t)(buffer_size - pos),
                           "\",\"uid\":\"");
        if (written < 0 || written >= buffer_size - pos) return -1;
        pos += written;
        pos = json_escape(buffer, buffer_size, pos, devices[i].uid);
        if (pos < 0) return -1;
        written = snprintf(buffer + pos, (size_t)(buffer_size - pos),
                           "\",\"input\":%d,\"output\":%d,\"sampleRate\":%.0f}",
                           devices[i].input_channels,
                           devices[i].output_channels,
                           devices[i].sample_rate);
        if (written < 0 || written >= buffer_size - pos) return -1;
        pos += written;
    }

    if (pos >= buffer_size - 1) return -1;
    buffer[pos++] = ']';
    buffer[pos] = '\0';
    return pos;
}

static void delay_free(HDelay *delay) {
    free(delay->buffer);
    delay->buffer = NULL;
    delay->length = 0;
}

static int delay_init(HDelay *delay, int length) {
    delay->length = length > 0 ? length : 1;
    delay->pos = 0;
    delay->feedback = 0.0f;
    delay->buffer = (float *)calloc((size_t)delay->length, sizeof(float));
    return delay->buffer ? 0 : -1;
}

static void reverb_free(HReverb *reverb) {
    for (int i = 0; i < REVERB_COMBS; i++) delay_free(&reverb->combs[i]);
    for (int i = 0; i < REVERB_ALLPASSES; i++) delay_free(&reverb->allpasses[i]);
}

static int reverb_init(HReverb *reverb, int sample_rate) {
    static const int comb_delays[REVERB_COMBS] = {1116, 1188, 1277, 1356};
    static const int allpass_delays[REVERB_ALLPASSES] = {556, 441};
    double scale = (double)sample_rate / 44100.0;

    memset(reverb, 0, sizeof(*reverb));
    reverb->sample_rate = sample_rate;
    reverb->damping = 0.3f;
    for (int i = 0; i < REVERB_COMBS; i++) {
        int length = (int)((double)comb_delays[i] * scale);
        if (length < 1) length = 1;
        if (delay_init(&reverb->combs[i], length) != 0) {
            reverb_free(reverb);
            return -1;
        }
    }
    for (int i = 0; i < REVERB_ALLPASSES; i++) {
        int length = (int)((double)allpass_delays[i] * scale);
        if (length < 1) length = 1;
        if (delay_init(&reverb->allpasses[i], length) != 0) {
            reverb_free(reverb);
            return -1;
        }
        reverb->allpasses[i].feedback = 0.5f;
    }
    return 0;
}

static void reverb_set_preset(HReverb *reverb, int preset, float wet) {
    float feedback = 0.0f;
    float damping = 0.3f;
    switch (preset) {
        case 1: feedback = 0.72f; damping = 0.45f; break;
        case 2: feedback = 0.84f; damping = 0.30f; break;
        case 3: feedback = 0.91f; damping = 0.22f; break;
        default: feedback = 0.0f; break;
    }
    if (wet < 0.0f) wet = 0.0f;
    if (wet > 0.6f) wet = 0.6f;
    for (int i = 0; i < REVERB_COMBS; i++) {
        reverb->combs[i].feedback = feedback;
    }
    reverb->damping = damping;
}

static inline float reverb_process(HReverb *reverb, float input, float wet) {
    float acc = 0.0f;
    for (int i = 0; i < REVERB_COMBS; i++) {
        HDelay *comb = &reverb->combs[i];
        float delayed = comb->buffer[comb->pos];
        comb->buffer[comb->pos] = input + delayed * comb->feedback;
        if (++comb->pos >= comb->length) comb->pos = 0;
        acc += delayed;
    }
    acc *= 1.0f / (float)REVERB_COMBS;

    for (int i = 0; i < REVERB_ALLPASSES; i++) {
        HDelay *allpass = &reverb->allpasses[i];
        float delayed = allpass->buffer[allpass->pos];
        allpass->buffer[allpass->pos] = acc + delayed * allpass->feedback;
        if (++allpass->pos >= allpass->length) allpass->pos = 0;
        acc = delayed - acc;
    }

    reverb->damp_state = reverb->damp_state * reverb->damping +
                         acc * (1.0f - reverb->damping);
    return input * (1.0f - wet) + reverb->damp_state * wet;
}

static inline float biquad_process(HBiquad *b, float x) {
    float y = b->b0 * x + b->z1;
    b->z1 = b->b1 * x - b->a1 * y + b->z2;
    b->z2 = b->b2 * x - b->a2 * y;
    return y;
}

static void biquad_set(HBiquad *b, float b0, float b1, float b2,
                       float a0, float a1, float a2) {
    if (a0 == 0.0f) {
        b->b0 = 1.0f;
        b->b1 = b->b2 = b->a1 = b->a2 = 0.0f;
        return;
    }
    b->b0 = b0 / a0;
    b->b1 = b1 / a0;
    b->b2 = b2 / a0;
    b->a1 = a1 / a0;
    b->a2 = a2 / a0;
}

static void design_low_shelf(HBiquad *b, float fs, float f0, float db) {
    const float pi = 3.14159265358979323846f;
    float A = powf(10.0f, db / 40.0f);
    float w0 = 2.0f * pi * f0 / fs;
    float cw = cosf(w0);
    float sw = sinf(w0);
    float alpha = 0.70710678f * sw; /* S = 1 */
    float sqrtA = sqrtf(A);

    float b0 = A * ((A + 1.0f) - (A - 1.0f) * cw + 2.0f * sqrtA * alpha);
    float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cw);
    float b2 = A * ((A + 1.0f) - (A - 1.0f) * cw - 2.0f * sqrtA * alpha);
    float a0 = (A + 1.0f) + (A - 1.0f) * cw + 2.0f * sqrtA * alpha;
    float a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cw);
    float a2 = (A + 1.0f) + (A - 1.0f) * cw - 2.0f * sqrtA * alpha;
    biquad_set(b, b0, b1, b2, a0, a1, a2);
}

static void design_peaking(HBiquad *b, float fs, float f0, float db, float q) {
    const float pi = 3.14159265358979323846f;
    float A = powf(10.0f, db / 40.0f);
    float w0 = 2.0f * pi * f0 / fs;
    float cw = cosf(w0);
    float sw = sinf(w0);
    float alpha = sw / (2.0f * q);

    float b0 = 1.0f + alpha * A;
    float b1 = -2.0f * cw;
    float b2 = 1.0f - alpha * A;
    float a0 = 1.0f + alpha / A;
    float a1 = -2.0f * cw;
    float a2 = 1.0f - alpha / A;
    biquad_set(b, b0, b1, b2, a0, a1, a2);
}

static void design_high_shelf(HBiquad *b, float fs, float f0, float db) {
    const float pi = 3.14159265358979323846f;
    float A = powf(10.0f, db / 40.0f);
    float w0 = 2.0f * pi * f0 / fs;
    float cw = cosf(w0);
    float sw = sinf(w0);
    float alpha = 0.70710678f * sw;
    float sqrtA = sqrtf(A);

    float b0 = A * ((A + 1.0f) + (A - 1.0f) * cw + 2.0f * sqrtA * alpha);
    float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cw);
    float b2 = A * ((A + 1.0f) + (A - 1.0f) * cw - 2.0f * sqrtA * alpha);
    float a0 = (A + 1.0f) - (A - 1.0f) * cw + 2.0f * sqrtA * alpha;
    float a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cw);
    float a2 = (A + 1.0f) - (A - 1.0f) * cw - 2.0f * sqrtA * alpha;
    biquad_set(b, b0, b1, b2, a0, a1, a2);
}

static void update_eq_coefficients(HAEngine *engine,
                                   float low_db, float mid_db, float high_db) {
    float fs = (float)engine->sample_rate;
    design_low_shelf(&engine->eq_low, fs, 120.0f, low_db);
    design_peaking(&engine->eq_mid, fs, 1000.0f, mid_db, 1.0f);
    design_high_shelf(&engine->eq_high, fs, 6000.0f, high_db);
    engine->eq_last_low = low_db;
    engine->eq_last_mid = mid_db;
    engine->eq_last_high = high_db;
}

static inline float compressor_process(HAEngine *engine, float x,
                                       float threshold_db, float ratio) {
    float absx = fabsf(x);
    float env_coefficient = absx > engine->comp_env
        ? engine->comp_attack
        : engine->comp_release;
    engine->comp_env += (absx - engine->comp_env) * env_coefficient;

    float db = 20.0f * log10f(engine->comp_env + 1e-9f);
    float over = db - threshold_db;
    float gain_db = over > 0.0f ? -over * (1.0f - 1.0f / ratio) : 0.0f;
    float target = powf(10.0f, gain_db / 20.0f);
    float gain_coefficient = target < engine->comp_gain
        ? engine->comp_attack
        : engine->comp_release;
    engine->comp_gain += (target - engine->comp_gain) * gain_coefficient;
    return x * engine->comp_gain;
}

static inline float soft_limit(float x) {
    const float threshold = 0.8912509f; /* -1 dBFS */
    if (x > threshold) {
        float excess = x - threshold;
        return threshold + excess / (1.0f + excess * 8.0f);
    }
    if (x < -threshold) {
        float excess = -x - threshold;
        return -threshold - excess / (1.0f + excess * 8.0f);
    }
    return x;
}

static OSStatus overload_listener(AudioObjectID objectID,
                                   UInt32 number_addresses,
                                   const AudioObjectPropertyAddress *addresses,
                                   void *client_data) {
    (void)objectID;
    (void)number_addresses;
    (void)addresses;
    HAEngine *engine = (HAEngine *)client_data;
    if (engine) {
        atomic_fetch_add_explicit(&engine->overload_count, 1, memory_order_relaxed);
    }
    return noErr;
}

static void install_overload_listener(HAEngine *engine) {
    if (!engine) return;
    AudioObjectPropertyAddress address = {
        kAudioDeviceProcessorOverload,
        kAudioObjectPropertyScopeGlobal,
        0
    };
    AudioDeviceID candidates[2] = { engine->aggregate_device, engine->output_device };
    for (int i = 0; i < 2; i++) {
        AudioDeviceID device = candidates[i];
        if (device == kAudioObjectUnknown) continue;
        OSStatus status = AudioObjectAddPropertyListener(
            device, &address, overload_listener, engine);
        if (status == noErr) {
            engine->overload_device = device;
            engine->overload_listener_installed = 1;
            return;
        }
    }
}

static OSStatus render_callback(void *inRefCon,
                                AudioUnitRenderActionFlags *ioActionFlags,
                                const AudioTimeStamp *inTimeStamp,
                                UInt32 inBusNumber,
                                UInt32 inNumberFrames,
                                AudioBufferList *ioData) {
    HAEngine *engine = (HAEngine *)inRefCon;
    (void)inBusNumber;

    if (!engine || !ioData) return noErr;

    if (inNumberFrames > MAX_FRAMES) {
        for (UInt32 b = 0; b < ioData->mNumberBuffers; b++) {
            memset(ioData->mBuffers[b].mData, 0, ioData->mBuffers[b].mDataByteSize);
        }
        return noErr;
    }

    engine->input_abl->mBuffers[0].mDataByteSize = inNumberFrames * sizeof(float);
    OSStatus status = AudioUnitRender(engine->unit, ioActionFlags, inTimeStamp,
                                      1, inNumberFrames, engine->input_abl);
    if (status != noErr) {
        for (UInt32 b = 0; b < ioData->mNumberBuffers; b++) {
            memset(ioData->mBuffers[b].mData, 0, ioData->mBuffers[b].mDataByteSize);
        }
        return noErr;
    }

    atomic_fetch_add_explicit(&engine->callback_count, 1, memory_order_relaxed);

    float *input = engine->input_buffer;
    float *mix = engine->mix_buffer;
    int preset = atomic_load_explicit(&engine->reverb_preset, memory_order_relaxed);
    float wet = atomic_load_explicit(&engine->reverb_wet, memory_order_relaxed);
    float input_gain = atomic_load_explicit(&engine->input_gain, memory_order_relaxed);
    float output_gain = atomic_load_explicit(&engine->output_gain, memory_order_relaxed);
    int muted = atomic_load_explicit(&engine->muted, memory_order_relaxed);
    int highpass = atomic_load_explicit(&engine->highpass_enabled, memory_order_relaxed);
    int gate = atomic_load_explicit(&engine->gate_enabled, memory_order_relaxed);
    float gate_threshold = atomic_load_explicit(&engine->gate_threshold, memory_order_relaxed);
    int eq_enabled = atomic_load_explicit(&engine->eq_enabled, memory_order_relaxed);
    float eq_low_db = atomic_load_explicit(&engine->eq_low_db, memory_order_relaxed);
    float eq_mid_db = atomic_load_explicit(&engine->eq_mid_db, memory_order_relaxed);
    float eq_high_db = atomic_load_explicit(&engine->eq_high_db, memory_order_relaxed);
    int comp_enabled = atomic_load_explicit(&engine->comp_enabled, memory_order_relaxed);
    float comp_threshold = atomic_load_explicit(&engine->comp_threshold_db, memory_order_relaxed);
    float comp_ratio = atomic_load_explicit(&engine->comp_ratio, memory_order_relaxed);
    if (comp_ratio < 1.01f) comp_ratio = 1.01f;

    if (eq_enabled &&
        (eq_enabled != engine->eq_last_enabled ||
         fabsf(eq_low_db - engine->eq_last_low) > 0.01f ||
         fabsf(eq_mid_db - engine->eq_last_mid) > 0.01f ||
         fabsf(eq_high_db - engine->eq_last_high) > 0.01f)) {
        update_eq_coefficients(engine, eq_low_db, eq_mid_db, eq_high_db);
    }
    engine->eq_last_enabled = eq_enabled;
    if (!comp_enabled) {
        engine->comp_gain = 1.0f;
    }

    double input_sum = 0.0;
    double output_sum = 0.0;
    float input_peak = 0.0f;
    float output_peak = 0.0f;
    int clipped = 0;

    for (UInt32 i = 0; i < inNumberFrames; i++) {
        float sample = input[i] * input_gain;
        float input_abs = fabsf(sample);
        if (input_abs > input_peak) input_peak = input_abs;
        input_sum += (double)sample * (double)sample;

        if (highpass) {
            float filtered = engine->hp_a *
                (engine->hp_y1 + sample - engine->hp_x1);
            engine->hp_x1 = sample;
            engine->hp_y1 = filtered;
            sample = filtered;
        }

        if (gate) {
            float gate_abs = fabsf(sample);
            /* Track the quiet floor so the expander adapts to the room
               instead of using a fixed threshold forever. */
            if (gate_abs < engine->gate_noise_floor) {
                engine->gate_noise_floor +=
                    (gate_abs - engine->gate_noise_floor) * 0.001f;
            } else {
                engine->gate_noise_floor *= 1.0000005f;
            }
            float threshold = gate_threshold;
            float adaptive = engine->gate_noise_floor * 2.5f;
            if (adaptive > threshold) threshold = adaptive;
            if (threshold < 1e-6f) threshold = 1e-6f;

            engine->gate_env = gate_abs > engine->gate_env
                ? gate_abs
                : engine->gate_env * engine->gate_env_release;

            float level_db = 20.0f * log10f(engine->gate_env + 1e-9f);
            float threshold_db = 20.0f * log10f(threshold + 1e-9f);
            float target = 1.0f;
            if (level_db < threshold_db) {
                float below = threshold_db - level_db;
                float reduction_db = below * 0.7f;
                if (reduction_db > 30.0f) reduction_db = 30.0f;
                target = powf(10.0f, -reduction_db / 20.0f);
            }
            float coefficient = target < engine->gate_gain
                ? engine->gate_attack
                : engine->gate_release;
            engine->gate_gain += (target - engine->gate_gain) * coefficient;
            sample *= engine->gate_gain;
        }

        if (eq_enabled) {
            sample = biquad_process(&engine->eq_low, sample);
            sample = biquad_process(&engine->eq_mid, sample);
            sample = biquad_process(&engine->eq_high, sample);
        }

        if (comp_enabled) {
            sample = compressor_process(engine, sample, comp_threshold, comp_ratio);
        }

        sample = hs_process(engine->feedback_suppressor, sample);

        float output = preset > 0
            ? reverb_process(&engine->reverb, sample, wet)
            : sample;
        output *= output_gain;
        if (muted) output = 0.0f;

        float output_abs = fabsf(output);
        if (output_abs > output_peak) output_peak = output_abs;
        if (output > 1.0f || output < -1.0f) clipped++;

        output = soft_limit(output);
        if (output > 1.0f) output = 1.0f;
        if (output < -1.0f) output = -1.0f;
        output_sum += (double)output * (double)output;
        mix[i] = output;
    }

    if (clipped > 0) {
        atomic_fetch_add_explicit(&engine->clip_count, clipped, memory_order_relaxed);
    }
    atomic_store_explicit(&engine->input_peak, input_peak, memory_order_relaxed);
    atomic_store_explicit(&engine->output_peak, output_peak, memory_order_relaxed);
    atomic_store_explicit(&engine->input_level,
                          sqrtf((float)(input_sum / (double)inNumberFrames)),
                          memory_order_relaxed);
    atomic_store_explicit(&engine->output_level,
                          sqrtf((float)(output_sum / (double)inNumberFrames)),
                          memory_order_relaxed);

    if (ioData->mNumberBuffers == 1) {
        AudioBuffer *buffer = &ioData->mBuffers[0];
        float *output = (float *)buffer->mData;
        UInt32 channels = buffer->mNumberChannels;
        if (channels <= 1) {
            memcpy(output, mix, inNumberFrames * sizeof(float));
        } else {
            for (UInt32 i = 0; i < inNumberFrames; i++) {
                for (UInt32 c = 0; c < channels; c++) {
                    output[i * channels + c] = mix[i];
                }
            }
        }
    } else {
        for (UInt32 b = 0; b < ioData->mNumberBuffers; b++) {
            memcpy(ioData->mBuffers[b].mData, mix, inNumberFrames * sizeof(float));
        }
    }

    return noErr;
}

static int set_buffer_frame_size(AudioDeviceID device, int frames) {
    if (device == kAudioObjectUnknown || frames <= 0) return 0;
    UInt32 value = (UInt32)frames;
    AudioObjectPropertyAddress addr = {
        kAudioDevicePropertyBufferFrameSize,
        kAudioObjectPropertyScopeGlobal,
        0
    };
    AudioObjectSetPropertyData(device, &addr, 0, NULL, sizeof(value), &value);
    UInt32 actual = 0;
    UInt32 size = sizeof(actual);
    OSStatus status = AudioObjectGetPropertyData(
        device, &addr, 0, NULL, &size, &actual);
    return status == noErr ? (int)actual : frames;
}

static OSStatus create_aggregate_device(AudioDeviceID input_device,
                                        AudioDeviceID output_device,
                                        AudioDeviceID *out_device,
                                        char *error,
                                        size_t error_size) {
    CFStringRef input_uid = copy_device_uid(input_device);
    CFStringRef output_uid = copy_device_uid(output_device);
    if (!input_uid || !output_uid) {
        snprintf(error, error_size, "Could not read device UIDs");
        if (input_uid) CFRelease(input_uid);
        if (output_uid) CFRelease(output_uid);
        return kAudioHardwareUnspecifiedError;
    }

    char uid[256];
    snprintf(uid, sizeof(uid), "com.breeze303.earcue.aggregate.%d.%llu",
             (int)getpid(), (unsigned long long)mach_absolute_time());
    CFStringRef aggregate_uid = CFStringCreateWithCString(
        NULL, uid, kCFStringEncodingUTF8);
    CFStringRef aggregate_name = CFSTR("EarCue Aggregate");

    CFMutableDictionaryRef description = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    /* The kAudioAggregateDevice*Key constants are C strings, not CFStrings. */
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceNameKey),
                         aggregate_name);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceUIDKey),
                         aggregate_uid);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceIsPrivateKey),
                         kCFBooleanTrue);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceMainSubDeviceKey),
                         output_uid);

    CFMutableArrayRef subdevices = CFArrayCreateMutable(
        NULL, 2, &kCFTypeArrayCallBacks);

    CFMutableDictionaryRef input_sub = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(input_sub, CFSTR(kAudioSubDeviceUIDKey), input_uid);
    CFDictionarySetValue(input_sub, CFSTR(kAudioSubDeviceDriftCompensationKey),
                         kCFBooleanTrue);
    CFArrayAppendValue(subdevices, input_sub);

    CFMutableDictionaryRef output_sub = CFDictionaryCreateMutable(
        NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(output_sub, CFSTR(kAudioSubDeviceUIDKey), output_uid);
    CFDictionarySetValue(output_sub, CFSTR(kAudioSubDeviceDriftCompensationKey),
                         kCFBooleanFalse);
    CFArrayAppendValue(subdevices, output_sub);

    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceSubDeviceListKey),
                         subdevices);

    AudioDeviceID aggregate = kAudioObjectUnknown;
    OSStatus status = AudioHardwareCreateAggregateDevice(description, &aggregate);
    if (status != noErr) {
        snprintf(error, error_size,
                 "AudioHardwareCreateAggregateDevice failed (OSStatus %d)",
                 (int)status);
    } else {
        *out_device = aggregate;
    }

    CFRelease(output_sub);
    CFRelease(input_sub);
    CFRelease(subdevices);
    CFRelease(description);
    CFRelease(aggregate_uid);
    CFRelease(input_uid);
    CFRelease(output_uid);
    return status;
}

static int setup_audio_unit(HAEngine *engine) {
    AudioComponentDescription description = {
        .componentType = kAudioUnitType_Output,
        .componentSubType = kAudioUnitSubType_HALOutput,
        .componentManufacturer = kAudioUnitManufacturer_Apple,
        .componentFlags = 0,
        .componentFlagsMask = 0
    };
    AudioComponent component = AudioComponentFindNext(NULL, &description);
    if (!component) {
        set_error(engine, "Could not find the HAL output audio unit");
        return -1;
    }

    OSStatus status = AudioComponentInstanceNew(component, &engine->unit);
    if (status != noErr) {
        return set_error_status(engine, status, "AudioComponentInstanceNew");
    }

    UInt32 enable = 1;
    status = AudioUnitSetProperty(engine->unit,
                                  kAudioOutputUnitProperty_EnableIO,
                                  kAudioUnitScope_Input, 1,
                                  &enable, sizeof(enable));
    if (status != noErr) {
        return set_error_status(engine, status, "Enable input IO");
    }

    status = AudioUnitSetProperty(engine->unit,
                                  kAudioOutputUnitProperty_EnableIO,
                                  kAudioUnitScope_Output, 0,
                                  &enable, sizeof(enable));
    if (status != noErr) {
        return set_error_status(engine, status, "Enable output IO");
    }

    AudioDeviceID device = engine->aggregate_device;
    status = AudioUnitSetProperty(engine->unit,
                                  kAudioOutputUnitProperty_CurrentDevice,
                                  kAudioUnitScope_Global, 0,
                                  &device, sizeof(device));
    if (status != noErr) {
        return set_error_status(engine, status, "Set current device");
    }

    AudioStreamBasicDescription format;
    memset(&format, 0, sizeof(format));
    format.mSampleRate = engine->sample_rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsFloat |
                          kAudioFormatFlagIsPacked |
                          kAudioFormatFlagIsNonInterleaved;
    format.mBytesPerPacket = sizeof(float);
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = sizeof(float);
    format.mBitsPerChannel = 32;

    /* AUHAL client-side formats:
       - input element 1, output scope = data delivered to the client
       - output element 0, input scope  = data received from the client */
    format.mChannelsPerFrame = (UInt32)engine->in_channels;
    status = AudioUnitSetProperty(engine->unit,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Output, 1,
                                  &format, sizeof(format));
    if (status != noErr) {
        return set_error_status(engine, status, "Set input stream format");
    }

    format.mChannelsPerFrame = (UInt32)engine->out_channels;
    status = AudioUnitSetProperty(engine->unit,
                                  kAudioUnitProperty_StreamFormat,
                                  kAudioUnitScope_Input, 0,
                                  &format, sizeof(format));
    if (status != noErr) {
        return set_error_status(engine, status, "Set output stream format");
    }

    UInt32 max_frames = MAX_FRAMES;
    status = AudioUnitSetProperty(engine->unit,
                                  kAudioUnitProperty_MaximumFramesPerSlice,
                                  kAudioUnitScope_Global, 0,
                                  &max_frames, sizeof(max_frames));
    if (status != noErr) {
        return set_error_status(engine, status, "Set maximum frames");
    }

    AURenderCallbackStruct callback = {
        .inputProc = render_callback,
        .inputProcRefCon = engine
    };
    status = AudioUnitSetProperty(engine->unit,
                                  kAudioUnitProperty_SetRenderCallback,
                                  kAudioUnitScope_Input, 0,
                                  &callback, sizeof(callback));
    if (status != noErr) {
        return set_error_status(engine, status, "Set render callback");
    }

    status = AudioUnitInitialize(engine->unit);
    if (status != noErr) {
        return set_error_status(engine, status, "AudioUnitInitialize");
    }

    return 0;
}

HAEngine *ha_engine_create(uint32_t input_device,
                           uint32_t output_device,
                           int buffer_frames,
                           double sample_rate) {
    HAEngine *engine = (HAEngine *)calloc(1, sizeof(HAEngine));
    if (!engine) return NULL;

    engine->input_device = input_device;
    engine->output_device = output_device;
    engine->aggregate_device = kAudioObjectUnknown;
    engine->sample_rate = sample_rate > 0 ? sample_rate : 48000.0;
    engine->requested_buffer_frames = buffer_frames > 0 ? buffer_frames : 32;
    engine->in_channels = 1;
    engine->out_channels = 2;
    engine->error[0] = '\0';
    engine->overload_device = kAudioObjectUnknown;

    atomic_store_explicit(&engine->input_gain, 1.0f, memory_order_relaxed);
    atomic_store_explicit(&engine->output_gain, 1.0f, memory_order_relaxed);
    atomic_store_explicit(&engine->muted, 0, memory_order_relaxed);
    atomic_store_explicit(&engine->highpass_enabled, 0, memory_order_relaxed);
    atomic_store_explicit(&engine->gate_enabled, 0, memory_order_relaxed);
    atomic_store_explicit(&engine->gate_threshold,
                          powf(10.0f, -50.0f / 20.0f), memory_order_relaxed);
    atomic_store_explicit(&engine->clip_count, 0, memory_order_relaxed);
    atomic_store_explicit(&engine->callback_count, 0, memory_order_relaxed);
    atomic_store_explicit(&engine->overload_count, 0, memory_order_relaxed);

    engine->gate_gain = 1.0f;
    engine->gate_attack = 1.0f - expf(-1.0f / (0.001f * (float)engine->sample_rate));
    engine->gate_release = 1.0f - expf(-1.0f / (0.100f * (float)engine->sample_rate));
    engine->gate_env_release = expf(-1.0f / (0.050f * (float)engine->sample_rate));

    atomic_store_explicit(&engine->eq_enabled, 0, memory_order_relaxed);
    atomic_store_explicit(&engine->eq_low_db, 0.0f, memory_order_relaxed);
    atomic_store_explicit(&engine->eq_mid_db, 0.0f, memory_order_relaxed);
    atomic_store_explicit(&engine->eq_high_db, 0.0f, memory_order_relaxed);
    atomic_store_explicit(&engine->comp_enabled, 0, memory_order_relaxed);
    atomic_store_explicit(&engine->comp_threshold_db, -24.0f, memory_order_relaxed);
    atomic_store_explicit(&engine->comp_ratio, 3.0f, memory_order_relaxed);
    engine->comp_env = 0.0f;
    engine->comp_gain = 1.0f;
    engine->comp_attack = 1.0f - expf(-1.0f / (0.010f * (float)engine->sample_rate));
    engine->comp_release = 1.0f - expf(-1.0f / (0.100f * (float)engine->sample_rate));
    engine->eq_last_enabled = 0;
    double highpass_rc = 1.0 / (2.0 * 3.14159265358979323846 * 80.0);
    double highpass_dt = 1.0 / engine->sample_rate;
    engine->hp_a = (float)(highpass_rc / (highpass_rc + highpass_dt));

    int input_channels = get_channel_count(input_device, kAudioObjectPropertyScopeInput);
    int output_channels = get_channel_count(output_device, kAudioObjectPropertyScopeOutput);
    if (input_channels < 1) {
        set_error(engine, "Selected input device has no input channels");
        return engine;
    }
    if (output_channels < 1) {
        set_error(engine, "Selected output device has no output channels");
        return engine;
    }
    engine->out_channels = output_channels >= 2 ? 2 : 1;

    engine->input_buffer = (float *)calloc(MAX_FRAMES, sizeof(float));
    engine->mix_buffer = (float *)calloc(MAX_FRAMES, sizeof(float));
    engine->input_abl = (AudioBufferList *)calloc(1, sizeof(AudioBufferList));
    if (!engine->input_buffer || !engine->mix_buffer || !engine->input_abl) {
        set_error(engine, "Could not allocate audio buffers");
        return engine;
    }
    engine->input_abl->mNumberBuffers = 1;
    engine->input_abl->mBuffers[0].mNumberChannels = 1;
    engine->input_abl->mBuffers[0].mData = engine->input_buffer;
    engine->input_abl->mBuffers[0].mDataByteSize = 0;

    if (reverb_init(&engine->reverb, (int)engine->sample_rate) != 0) {
        set_error(engine, "Could not allocate reverb buffers");
        return engine;
    }

    engine->feedback_suppressor = hs_create((int)engine->sample_rate);
    if (!engine->feedback_suppressor) {
        set_error(engine, "Could not allocate feedback suppressor");
        return engine;
    }
    engine->gate_noise_floor = 0.001f;

    if (input_device == output_device) {
        engine->aggregate_device = output_device;
    } else {
        char aggregate_error[256] = {0};
        OSStatus status = create_aggregate_device(
            input_device, output_device, &engine->aggregate_device,
            aggregate_error, sizeof(aggregate_error));
        if (status != noErr) {
            set_error(engine, "%s", aggregate_error);
            return engine;
        }
    }

    engine->actual_buffer_frames = set_buffer_frame_size(
        engine->aggregate_device, engine->requested_buffer_frames);

    if (setup_audio_unit(engine) != 0) {
        return engine;
    }

    install_overload_listener(engine);
    return engine;
}

int ha_engine_start(HAEngine *engine) {
    if (!engine) return -1;
    if (engine->error[0] != '\0') return -1;
    if (!engine->unit) {
        set_error(engine, "Audio unit is not initialized");
        return -1;
    }
    if (engine->running) return 0;

    OSStatus status = AudioUnitInitialize(engine->unit);
    if (status != noErr && status != kAudioUnitErr_Initialized) {
        return set_error_status(engine, status, "AudioUnitInitialize");
    }
    status = AudioOutputUnitStart(engine->unit);
    if (status != noErr) {
        return set_error_status(engine, status, "AudioOutputUnitStart");
    }
    engine->running = 1;
    return 0;
}

void ha_engine_stop(HAEngine *engine) {
    if (!engine || !engine->unit) return;
    if (engine->running) {
        AudioOutputUnitStop(engine->unit);
        AudioUnitUninitialize(engine->unit);
        engine->running = 0;
    }
}

void ha_engine_set_reverb(HAEngine *engine, int preset, float wet) {
    if (!engine) return;
    atomic_store_explicit(&engine->reverb_wet, wet, memory_order_relaxed);
    atomic_store_explicit(&engine->reverb_preset, preset, memory_order_relaxed);
    reverb_set_preset(&engine->reverb, preset, wet);
}

void ha_engine_set_gain(HAEngine *engine, float input_gain,
                        float output_gain, int muted) {
    if (!engine) return;
    if (input_gain < 0.0f) input_gain = 0.0f;
    if (input_gain > 16.0f) input_gain = 16.0f;
    if (output_gain < 0.0f) output_gain = 0.0f;
    if (output_gain > 8.0f) output_gain = 8.0f;
    atomic_store_explicit(&engine->input_gain, input_gain, memory_order_relaxed);
    atomic_store_explicit(&engine->output_gain, output_gain, memory_order_relaxed);
    atomic_store_explicit(&engine->muted, muted ? 1 : 0, memory_order_relaxed);
}

void ha_engine_set_cleanup(HAEngine *engine, int highpass, int gate,
                           float gate_threshold_db) {
    if (!engine) return;
    if (gate_threshold_db < -80.0f) gate_threshold_db = -80.0f;
    if (gate_threshold_db > 0.0f) gate_threshold_db = 0.0f;
    atomic_store_explicit(&engine->highpass_enabled, highpass ? 1 : 0,
                          memory_order_relaxed);
    atomic_store_explicit(&engine->gate_enabled, gate ? 1 : 0,
                          memory_order_relaxed);
    atomic_store_explicit(&engine->gate_threshold,
                          powf(10.0f, gate_threshold_db / 20.0f),
                          memory_order_relaxed);
}

void ha_engine_set_eq(HAEngine *engine, int enabled,
                      float low_db, float mid_db, float high_db) {
    if (!engine) return;
    if (low_db < -12.0f) low_db = -12.0f;
    if (low_db > 12.0f) low_db = 12.0f;
    if (mid_db < -12.0f) mid_db = -12.0f;
    if (mid_db > 12.0f) mid_db = 12.0f;
    if (high_db < -12.0f) high_db = -12.0f;
    if (high_db > 12.0f) high_db = 12.0f;
    atomic_store_explicit(&engine->eq_low_db, low_db, memory_order_relaxed);
    atomic_store_explicit(&engine->eq_mid_db, mid_db, memory_order_relaxed);
    atomic_store_explicit(&engine->eq_high_db, high_db, memory_order_relaxed);
    atomic_store_explicit(&engine->eq_enabled, enabled ? 1 : 0,
                          memory_order_relaxed);
}

void ha_engine_set_compressor(HAEngine *engine, int enabled,
                              float threshold_db, float ratio) {
    if (!engine) return;
    if (threshold_db < -60.0f) threshold_db = -60.0f;
    if (threshold_db > 0.0f) threshold_db = 0.0f;
    if (ratio < 1.0f) ratio = 1.0f;
    if (ratio > 10.0f) ratio = 10.0f;
    atomic_store_explicit(&engine->comp_threshold_db, threshold_db,
                          memory_order_relaxed);
    atomic_store_explicit(&engine->comp_ratio, ratio, memory_order_relaxed);
    atomic_store_explicit(&engine->comp_enabled, enabled ? 1 : 0,
                          memory_order_relaxed);
}

void ha_engine_set_feedback_suppression(HAEngine *engine,
                                        int enabled, int sensitivity) {
    if (!engine || !engine->feedback_suppressor) return;
    hs_set_enabled(engine->feedback_suppressor, enabled, sensitivity);
}

int ha_engine_feedback_notch_count(HAEngine *engine) {
    if (!engine || !engine->feedback_suppressor) return 0;
    return hs_notch_count(engine->feedback_suppressor);
}

float ha_engine_input_peak(HAEngine *engine) {
    return engine ? atomic_load_explicit(&engine->input_peak, memory_order_relaxed) : 0.0f;
}

float ha_engine_output_level(HAEngine *engine) {
    return engine ? atomic_load_explicit(&engine->output_level, memory_order_relaxed) : 0.0f;
}

float ha_engine_output_peak(HAEngine *engine) {
    return engine ? atomic_load_explicit(&engine->output_peak, memory_order_relaxed) : 0.0f;
}

unsigned long long ha_engine_callback_count(HAEngine *engine) {
    return engine ? atomic_load_explicit(&engine->callback_count, memory_order_relaxed) : 0;
}

int ha_engine_overload_count(HAEngine *engine) {
    return engine ? atomic_load_explicit(&engine->overload_count, memory_order_relaxed) : 0;
}

void ha_engine_reset_overload_count(HAEngine *engine) {
    if (engine) atomic_store_explicit(&engine->overload_count, 0, memory_order_relaxed);
}

int ha_engine_clip_count(HAEngine *engine) {
    return engine ? atomic_load_explicit(&engine->clip_count, memory_order_relaxed) : 0;
}

void ha_engine_reset_clip_count(HAEngine *engine) {
    if (engine) atomic_store_explicit(&engine->clip_count, 0, memory_order_relaxed);
}

float ha_engine_input_level(HAEngine *engine) {
    if (!engine) return 0.0f;
    return atomic_load_explicit(&engine->input_level, memory_order_relaxed);
}

const char *ha_engine_error(HAEngine *engine) {
    return engine ? engine->error : "Could not allocate audio engine";
}

int ha_engine_latency_frames(HAEngine *engine) {
    return engine ? engine->actual_buffer_frames : 0;
}

double ha_engine_sample_rate(HAEngine *engine) {
    return engine ? engine->sample_rate : 0.0;
}

int ha_engine_is_running(HAEngine *engine) {
    return engine ? engine->running : 0;
}

void ha_engine_destroy(HAEngine *engine) {
    if (!engine) return;
    ha_engine_stop(engine);
    if (engine->overload_listener_installed) {
        AudioObjectPropertyAddress address = {
            kAudioDeviceProcessorOverload,
            kAudioObjectPropertyScopeGlobal,
            0
        };
        AudioObjectRemovePropertyListener(
            engine->overload_device, &address, overload_listener, engine);
        engine->overload_listener_installed = 0;
    }
    if (engine->unit) {
        AudioComponentInstanceDispose(engine->unit);
        engine->unit = NULL;
    }
    if (engine->aggregate_device != kAudioObjectUnknown &&
        engine->aggregate_device != engine->output_device &&
        engine->aggregate_device != engine->input_device) {
        AudioHardwareDestroyAggregateDevice(engine->aggregate_device);
    }
    reverb_free(&engine->reverb);
    hs_destroy(engine->feedback_suppressor);
    free(engine->input_abl);
    free(engine->input_buffer);
    free(engine->mix_buffer);
    free(engine);
}
