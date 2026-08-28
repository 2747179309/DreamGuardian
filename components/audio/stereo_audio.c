#include "stereo_audio.h"

#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#define AUDIO_FRAME_COUNT 512
#define AUDIO_TASK_STACK_SIZE 4096
#define AUDIO_TASK_PRIORITY 5
#define AUDIO_PI 3.14159265358979323846f
#define AUDIO_WAV_PATH_MAX 96
#define AUDIO_CODEC_OUTPUT_VOLUME 88
#define SINE_TABLE_SIZE 256

typedef enum {
    AUDIO_MODE_SILENT = 0,
    AUDIO_MODE_PINK_NOISE,
    AUDIO_MODE_STEREO_BREATHING,
    AUDIO_MODE_WAKE_MUSIC,
    AUDIO_MODE_SLEEP_MUSIC,
    AUDIO_MODE_WAV_FILE,
    AUDIO_MODE_SLEEP_ASSIST,
    AUDIO_MODE_WAKE_ACK,
    AUDIO_MODE_TEST_LEFT,
    AUDIO_MODE_TEST_RIGHT,
    AUDIO_MODE_TEST_BOTH,
} audio_mode_t;

typedef enum {
    SLEEP_MUSIC_PRESET_PPM = 0,
    SLEEP_MUSIC_PRESET_OCEAN,
    SLEEP_MUSIC_PRESET_FOREST,
    SLEEP_MUSIC_PRESET_RAIN,
    SLEEP_MUSIC_PRESET_ZEN,
    SLEEP_MUSIC_PRESET_EMPTY,
} sleep_music_preset_t;

typedef struct {
    uint8_t pad;
    uint8_t piano;
    uint8_t wave;
    uint8_t rain;
    uint8_t stream;
    uint8_t birds;
    uint8_t wind;
    uint8_t chimes;
} sleep_music_mix_t;

typedef struct {
    uint32_t data_offset;
    uint32_t data_size;
    uint32_t sample_rate_hz;
    uint16_t channels;
    uint16_t bits_per_sample;
} wav_info_t;

typedef struct {
    esp_codec_dev_handle_t speaker;
    audio_mode_t mode;
    intervention_action_t requested_action;
    uint8_t volume_percent;
    int64_t stop_at_us;
    uint32_t sample_rate_hz;
    uint32_t noise_state;
    int32_t pink_state;
    uint32_t tone_phase;
    uint32_t breath_phase;
    uint32_t ambient_phase;
    uint32_t bird_phase;
    uint32_t pad_phase_a;
    uint32_t pad_phase_b;
    uint32_t pad_phase_c;
    uint32_t piano_phase;
    uint32_t melody_sample;
    int32_t noise_lp_fast;
    int32_t noise_lp_slow;
    int32_t noise_lp_deep;
    float assist_volume;
    float assist_target_breath_bpm;
    float assist_pan_depth;
    float assist_pan_phase;
    float assist_breath_phase;
    bool assist_rhythm;
    uint32_t base_sample_rate_hz;
    uint32_t codec_sample_rate_hz;
    uint32_t wav_request_id;
    char wav_path[AUDIO_WAV_PATH_MAX];
    FILE *wav_file;
    wav_info_t wav_info;
    uint32_t wav_data_remaining;
    uint32_t wav_open_request_id;
    int32_t wav_lp_left;
    int32_t wav_lp_right;
    bool wav_loop;
    bool output_enabled;
    bool codec_open;
    sleep_music_preset_t sleep_music_preset;
    bool initialized;
    uint32_t write_calls;
    uint32_t write_errors;
    esp_err_t last_error;
    char last_result[64];
    portMUX_TYPE lock;
} audio_state_t;

static const char *TAG = "stereo_audio";
static audio_state_t s_audio = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

static int16_t s_sine_table[SINE_TABLE_SIZE];
static int16_t s_render_frames[AUDIO_FRAME_COUNT * 2];
static int16_t s_wav_samples[AUDIO_FRAME_COUNT * 2];
static StaticTask_t s_audio_task_tcb;
static StackType_t s_audio_task_stack[AUDIO_TASK_STACK_SIZE / sizeof(StackType_t)];

static void set_audio_result_locked(esp_err_t err, const char *result)
{
    s_audio.last_error = err;
    snprintf(s_audio.last_result, sizeof(s_audio.last_result),
             "%s", result ? result : esp_err_to_name(err));
}

static void set_audio_result(esp_err_t err, const char *result)
{
    portENTER_CRITICAL(&s_audio.lock);
    set_audio_result_locked(err, result);
    portEXIT_CRITICAL(&s_audio.lock);
}

static void audio_output_enable(bool enable)
{
    if (!s_audio.speaker) {
        return;
    }
    if (s_audio.output_enabled == enable) {
        return;
    }
    if (s_audio.codec_open) {
        if (enable) {
            (void)esp_codec_dev_set_out_vol(s_audio.speaker,
                                            AUDIO_CODEC_OUTPUT_VOLUME);
            (void)esp_codec_dev_set_out_mute(s_audio.speaker, false);
            /* Let the codec output settle before connecting the external PA.
             * The I2S task writes a silent pre-roll before reaching this path. */
            vTaskDelay(pdMS_TO_TICKS(8));
            (void)gpio_set_level(BSP_POWER_AMP_IO, 1);
        } else {
            (void)gpio_set_level(BSP_POWER_AMP_IO, 0);
            (void)esp_codec_dev_set_out_mute(s_audio.speaker, true);
        }
    }
    s_audio.output_enabled = enable;
}

static const sleep_music_mix_t SLEEP_MUSIC_MIXES[] = {
    [SLEEP_MUSIC_PRESET_PPM] = {.pad = 80, .piano = 60, .wave = 15, .rain = 0, .stream = 40, .birds = 30, .wind = 15, .chimes = 40},
    [SLEEP_MUSIC_PRESET_OCEAN] = {.pad = 90, .piano = 30, .wave = 80, .rain = 0, .stream = 0, .birds = 0, .wind = 20, .chimes = 0},
    [SLEEP_MUSIC_PRESET_FOREST] = {.pad = 40, .piano = 70, .wave = 0, .rain = 0, .stream = 60, .birds = 85, .wind = 40, .chimes = 50},
    [SLEEP_MUSIC_PRESET_RAIN] = {.pad = 60, .piano = 40, .wave = 0, .rain = 80, .stream = 20, .birds = 0, .wind = 30, .chimes = 0},
    [SLEEP_MUSIC_PRESET_ZEN] = {.pad = 85, .piano = 70, .wave = 0, .rain = 0, .stream = 0, .birds = 0, .wind = 10, .chimes = 85},
    [SLEEP_MUSIC_PRESET_EMPTY] = {.pad = 0, .piano = 0, .wave = 0, .rain = 0, .stream = 0, .birds = 0, .wind = 0, .chimes = 0},
};

static int16_t clamp_i16(int32_t sample)
{
    if (sample > INT16_MAX) {
        return INT16_MAX;
    }
    if (sample < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)sample;
}

static int16_t triangle_wave(uint32_t phase)
{
    uint16_t x = phase >> 16;
    int32_t value = (x < 32768) ? x : (65535 - x);
    return (int16_t)((value - 16384) * 2);
}

static void init_sine_table(void)
{
    for (size_t i = 0; i < SINE_TABLE_SIZE; ++i) {
        float phase = (2.0f * AUDIO_PI * (float)i) / (float)SINE_TABLE_SIZE;
        s_sine_table[i] = (int16_t)(sinf(phase) * 32767.0f);
    }
}

static int16_t sine_wave(uint32_t phase)
{
    uint8_t idx = phase >> 24;
    uint8_t next = (uint8_t)(idx + 1U);
    uint8_t frac = (phase >> 16) & 0xFFU;
    int32_t a = s_sine_table[idx];
    int32_t b = s_sine_table[next];
    return (int16_t)(a + (((b - a) * frac) >> 8));
}

static uint32_t phase_step_hz(uint32_t hz)
{
    return (uint32_t)(((uint64_t)hz * UINT32_MAX) / s_audio.sample_rate_hz);
}

static int32_t triangle_env(size_t pos, size_t length)
{
    if (length == 0 || pos >= length) {
        return 0;
    }
    size_t half = length / 2;
    if (half == 0) {
        return 32767;
    }
    size_t rising = pos < half ? pos : length - pos;
    return (int32_t)((rising * 32767U) / half);
}

static int16_t next_pink_sample(void)
{
    s_audio.noise_state = s_audio.noise_state * 1664525U + 1013904223U;
    int32_t white = (int16_t)(s_audio.noise_state >> 16);
    s_audio.pink_state += (white - s_audio.pink_state) >> 4;
    return clamp_i16((white >> 2) + s_audio.pink_state);
}

static void next_nature_noise(int32_t *soft, int32_t *deep, int32_t *air)
{
    s_audio.noise_state = s_audio.noise_state * 1664525U + 1013904223U;
    int32_t white = (int16_t)(s_audio.noise_state >> 16);
    s_audio.noise_lp_fast += (white - s_audio.noise_lp_fast) >> 3;
    s_audio.noise_lp_slow += (s_audio.noise_lp_fast - s_audio.noise_lp_slow) >> 5;
    s_audio.noise_lp_deep += (s_audio.noise_lp_slow - s_audio.noise_lp_deep) >> 6;
    if (soft) {
        *soft = s_audio.noise_lp_slow;
    }
    if (deep) {
        *deep = s_audio.noise_lp_deep;
    }
    if (air) {
        *air = white - s_audio.noise_lp_fast;
    }
}

static int32_t soft_limit(int32_t sample)
{
    const int32_t knee = 22000;
    int32_t sign = sample < 0 ? -1 : 1;
    int32_t mag = sample < 0 ? -sample : sample;
    if (mag > knee) {
        mag = knee + (mag - knee) / 5;
    }
    return sign * mag;
}

static void render_sleep_music_frame(const sleep_music_mix_t *mix, int32_t *left, int32_t *right)
{
    static const uint16_t notes_hz[] = {196, 247, 294, 330, 392, 330, 294, 247};
    int32_t soft = 0;
    int32_t deep = 0;
    int32_t air = 0;
    next_nature_noise(&soft, &deep, &air);

    int32_t wave_lfo = sine_wave(s_audio.breath_phase);
    int32_t wave_env = 15000 + ((wave_lfo + 32768) * 9000) / 65535;
    int32_t wave = (deep * wave_env) >> 15;
    int32_t rain = (air / 4) + (soft / 5);
    int32_t stream = (soft / 2) + (sine_wave(s_audio.ambient_phase) / 5);
    int32_t wind = (deep * (22000 + (sine_wave(s_audio.breath_phase >> 1) >> 1))) >> 15;
    int32_t nature = (wave * mix->wave) / 260 +
                      (rain * mix->rain) / 360 +
                      (stream * mix->stream) / 300 +
                      (wind * mix->wind) / 360;

    int32_t pad_mix = sine_wave(s_audio.pad_phase_a) +
                      (sine_wave(s_audio.pad_phase_b) * 2) / 3 +
                      sine_wave(s_audio.pad_phase_c) / 2;
    int32_t pad = (pad_mix * mix->pad) / 1350;

    uint32_t note_len = s_audio.sample_rate_hz * 5U;
    uint32_t note_index = (s_audio.melody_sample / note_len) %
                          (sizeof(notes_hz) / sizeof(notes_hz[0]));
    size_t note_pos = s_audio.melody_sample % note_len;
    int32_t note_env = triangle_env(note_pos, note_len);
    int32_t melody = (sine_wave(s_audio.piano_phase) * note_env * mix->piano) /
                     (32767 * 24);

    uint32_t cycle = s_audio.sample_rate_hz * 18U;
    uint32_t bird_len = s_audio.sample_rate_hz / 2U;
    uint32_t cycle_pos = s_audio.melody_sample % cycle;
    int32_t bird = 0;
    if (mix->birds > 0 &&
        cycle_pos > s_audio.sample_rate_hz * 4U &&
        cycle_pos < s_audio.sample_rate_hz * 4U + bird_len) {
        uint32_t bird_pos = cycle_pos - s_audio.sample_rate_hz * 4U;
        uint32_t chirp_hz = 1200U + (bird_pos * 500U) / bird_len;
        bird = ((sine_wave(s_audio.bird_phase) *
                 triangle_env(bird_pos, bird_len)) >> 15) * mix->birds / 240;
        s_audio.bird_phase += phase_step_hz(chirp_hz);
    }

    int32_t chime = 0;
    if (mix->chimes > 0 && (s_audio.melody_sample % (s_audio.sample_rate_hz * 22U)) < s_audio.sample_rate_hz) {
        uint32_t chime_pos = s_audio.melody_sample % s_audio.sample_rate_hz;
        chime = (sine_wave(s_audio.bird_phase + UINT32_MAX / 5U) *
                 triangle_env(chime_pos, s_audio.sample_rate_hz)) >> 15;
        chime = (chime * mix->chimes) / 260;
        s_audio.bird_phase += phase_step_hz(620);
    }

    int32_t l = nature + pad + melody + bird + chime;
    int32_t r = (nature * 9) / 10 + pad + (melody * 9) / 10 + (bird * 3) / 4 + (chime * 4) / 5;
    *left = soft_limit(l);
    *right = soft_limit(r);

    s_audio.pad_phase_a += phase_step_hz(98);
    s_audio.pad_phase_b += phase_step_hz(147);
    s_audio.pad_phase_c += phase_step_hz(196);
    s_audio.piano_phase += phase_step_hz(notes_hz[note_index]);
    s_audio.breath_phase += UINT32_MAX / (s_audio.sample_rate_hz * 9U);
    s_audio.ambient_phase += phase_step_hz(29);
    s_audio.melody_sample++;
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static esp_err_t wav_parse(FILE *file, wav_info_t *info)
{
    if (!file || !info) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));
    uint8_t header[12];
    if (fread(header, 1, sizeof(header), file) != sizeof(header)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    bool have_fmt = false;
    bool have_data = false;
    while (!have_data) {
        uint8_t chunk[8];
        if (fread(chunk, 1, sizeof(chunk), file) != sizeof(chunk)) {
            return ESP_ERR_INVALID_SIZE;
        }
        uint32_t chunk_size = read_le32(chunk + 4);
        long chunk_data = ftell(file);
        if (chunk_data < 0) {
            return ESP_FAIL;
        }

        if (memcmp(chunk, "fmt ", 4) == 0) {
            uint8_t fmt[16];
            if (chunk_size < sizeof(fmt) || fread(fmt, 1, sizeof(fmt), file) != sizeof(fmt)) {
                return ESP_ERR_INVALID_SIZE;
            }
            uint16_t audio_format = read_le16(fmt);
            info->channels = read_le16(fmt + 2);
            info->sample_rate_hz = read_le32(fmt + 4);
            info->bits_per_sample = read_le16(fmt + 14);
            if (audio_format != 1 || (info->channels != 1 && info->channels != 2) ||
                info->bits_per_sample != 16) {
                return ESP_ERR_NOT_SUPPORTED;
            }
            have_fmt = true;
        } else if (memcmp(chunk, "data", 4) == 0) {
            info->data_offset = (uint32_t)chunk_data;
            info->data_size = chunk_size;
            have_data = true;
            break;
        }

        long next = chunk_data + (long)chunk_size + (long)(chunk_size & 1U);
        if (fseek(file, next, SEEK_SET) != 0) {
            return ESP_ERR_INVALID_SIZE;
        }
    }

    return (have_fmt && have_data) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static esp_err_t wav_probe_path(const char *path, wav_info_t *info)
{
    FILE *file = fopen(path, "rb");
    if (!file) {
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    esp_err_t err = wav_parse(file, info);
    fclose(file);
    return err;
}

static void close_wav_file(void)
{
    if (s_audio.wav_file) {
        fclose(s_audio.wav_file);
        s_audio.wav_file = NULL;
    }
    memset(&s_audio.wav_info, 0, sizeof(s_audio.wav_info));
    s_audio.wav_data_remaining = 0;
    s_audio.wav_open_request_id = 0;
}

static esp_err_t ensure_codec_sample_rate(uint32_t sample_rate_hz)
{
    if (!s_audio.speaker || sample_rate_hz == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_audio.codec_open && s_audio.codec_sample_rate_hz == sample_rate_hz) {
        return ESP_OK;
    }

    if (s_audio.codec_open) {
        (void)esp_codec_dev_close(s_audio.speaker);
        s_audio.codec_open = false;
        s_audio.codec_sample_rate_hz = 0;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate_hz,
        .channel = 2,
        .bits_per_sample = 16,
    };
    int codec_err = esp_codec_dev_open(s_audio.speaker, &fs);
    if (codec_err != ESP_CODEC_DEV_OK) {
        set_audio_result((esp_err_t)codec_err, "speaker_reopen_failed");
        ESP_LOGE(TAG, "speaker reopen failed rate=%lu err=%d",
                 (unsigned long)sample_rate_hz, codec_err);
        return ESP_FAIL;
    }
    s_audio.codec_open = true;
    codec_err = esp_codec_dev_set_out_vol(s_audio.speaker, AUDIO_CODEC_OUTPUT_VOLUME);
    if (codec_err != ESP_CODEC_DEV_OK) {
        ESP_LOGW(TAG, "speaker volume set failed after reopen: %d", codec_err);
    }
    (void)esp_codec_dev_set_out_mute(s_audio.speaker, s_audio.output_enabled ? false : true);
    s_audio.codec_sample_rate_hz = sample_rate_hz;
    s_audio.sample_rate_hz = sample_rate_hz;
    ESP_LOGI(TAG, "speaker sample rate=%lu codec_vol=%u",
             (unsigned long)sample_rate_hz, AUDIO_CODEC_OUTPUT_VOLUME);
    return ESP_OK;
}

static esp_err_t open_wav_request(const char *path, uint32_t request_id)
{
    close_wav_file();
    FILE *file = fopen(path, "rb");
    if (!file) {
        ESP_LOGW(TAG, "WAV open failed path=%s errno=%d", path, errno);
        set_audio_result(errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL, "wav_open_failed");
        return errno == ENOENT ? ESP_ERR_NOT_FOUND : ESP_FAIL;
    }
    wav_info_t info = {0};
    esp_err_t err = wav_parse(file, &info);
    if (err != ESP_OK) {
        fclose(file);
        ESP_LOGW(TAG, "WAV parse failed path=%s err=%s", path, esp_err_to_name(err));
        set_audio_result(err, "wav_parse_failed");
        return err;
    }
    if (info.sample_rate_hz < 8000 || info.sample_rate_hz > 48000) {
        fclose(file);
        ESP_LOGW(TAG, "WAV sample rate %lu unsupported",
                 (unsigned long)info.sample_rate_hz);
        set_audio_result(ESP_ERR_NOT_SUPPORTED, "wav_rate_unsupported");
        return ESP_ERR_NOT_SUPPORTED;
    }
    err = ensure_codec_sample_rate(info.sample_rate_hz);
    if (err != ESP_OK) {
        fclose(file);
        return err;
    }
    if (fseek(file, (long)info.data_offset, SEEK_SET) != 0) {
        fclose(file);
        set_audio_result(ESP_FAIL, "wav_seek_failed");
        return ESP_FAIL;
    }
    s_audio.wav_file = file;
    s_audio.wav_info = info;
    s_audio.wav_data_remaining = info.data_size;
    s_audio.wav_open_request_id = request_id;
    s_audio.wav_lp_left = 0;
    s_audio.wav_lp_right = 0;
    set_audio_result(ESP_OK, "wav_opened");
    ESP_LOGI(TAG, "WAV playing path=%s channels=%u bytes=%lu",
             path, info.channels, (unsigned long)info.data_size);
    return ESP_OK;
}

static bool fill_wav_frames(int16_t *frames, size_t frame_count, uint8_t volume_percent)
{
    memset(frames, 0, frame_count * 2 * sizeof(int16_t));
    if (!s_audio.wav_file || s_audio.wav_data_remaining == 0) {
        return true;
    }

    size_t input_samples_per_frame = s_audio.wav_info.channels;
    size_t max_frames = s_audio.wav_data_remaining / (sizeof(int16_t) * input_samples_per_frame);
    size_t frames_to_read = frame_count < max_frames ? frame_count : max_frames;
    if (frames_to_read == 0) {
        return true;
    }

    size_t samples_to_read = frames_to_read * input_samples_per_frame;
    size_t samples_read = fread(s_wav_samples, sizeof(int16_t), samples_to_read, s_audio.wav_file);
    size_t complete_frames = samples_read / input_samples_per_frame;
    s_audio.wav_data_remaining -= (uint32_t)(samples_read * sizeof(int16_t));

    for (size_t i = 0; i < complete_frames; ++i) {
        int32_t left = s_wav_samples[i * input_samples_per_frame];
        int32_t right = input_samples_per_frame == 2 ? s_wav_samples[i * 2 + 1] : left;
        left = (left * volume_percent) / 100;
        right = (right * volume_percent) / 100;
        frames[i * 2] = clamp_i16(left);
        frames[i * 2 + 1] = clamp_i16(right);
    }

    return complete_frames < frame_count || s_audio.wav_data_remaining == 0 || ferror(s_audio.wav_file);
}

static void fill_audio_frames(int16_t *frames, size_t frame_count,
                              audio_mode_t mode, uint8_t volume_percent,
                              float assist_volume, float assist_target_breath_bpm,
                              float assist_pan_depth, bool assist_rhythm)
{
    const int32_t gain = (INT16_MAX * volume_percent) / 100;
    const uint32_t tone_step = (uint32_t)(((uint64_t)220 * UINT32_MAX) /
                                          s_audio.sample_rate_hz);
    const uint32_t breath_step = UINT32_MAX / (s_audio.sample_rate_hz * 8U);

    for (size_t i = 0; i < frame_count; ++i) {
        int32_t left = 0;
        int32_t right = 0;

        if (mode == AUDIO_MODE_PINK_NOISE) {
            int32_t sample = next_pink_sample();
            left = sample;
            right = sample;
        } else if (mode == AUDIO_MODE_STEREO_BREATHING) {
            int32_t tone = triangle_wave(s_audio.tone_phase);
            int32_t pan = triangle_wave(s_audio.breath_phase);
            int32_t left_gain = 32767 - pan;
            int32_t right_gain = 32767 + pan;
            left = (tone * left_gain) >> 16;
            right = (tone * right_gain) >> 16;
            s_audio.tone_phase += tone_step;
            s_audio.breath_phase += breath_step;
        } else if (mode == AUDIO_MODE_SLEEP_ASSIST) {
            sleep_music_preset_t preset = s_audio.sleep_music_preset;
            if (preset < 0 || preset >= (sizeof(SLEEP_MUSIC_MIXES) / sizeof(SLEEP_MUSIC_MIXES[0]))) {
                preset = SLEEP_MUSIC_PRESET_PPM;
            }
            const sleep_music_mix_t *mix = &SLEEP_MUSIC_MIXES[preset];
            float target = assist_target_breath_bpm;
            if (target < 4.0f) {
                target = 4.0f;
            }
            if (target > 20.0f) {
                target = 20.0f;
            }
            float period = 60.0f / target;
            float pan = sinf(s_audio.assist_pan_phase);
            float breath = 1.0f;
            if (assist_rhythm) {
                breath = 0.92f + 0.08f * 0.5f * (1.0f - cosf(s_audio.assist_breath_phase));
            }
            float music_weight = (assist_volume - 0.010f) / 0.025f;
            if (music_weight < 0.0f) {
                music_weight = 0.0f;
            }
            if (music_weight > 1.0f) {
                music_weight = 1.0f;
            }
            float noise_weight = 0.25f + (1.0f - music_weight) * 0.75f;

            int32_t music_left = 0;
            int32_t music_right = 0;
            render_sleep_music_frame(mix, &music_left, &music_right);
            int32_t noise = next_pink_sample() / 5;
            float left_gain = (0.95f + assist_pan_depth * pan) * breath;
            float right_gain = (0.95f - assist_pan_depth * pan) * breath;
            left = (int32_t)(((float)noise * noise_weight + (float)music_left * music_weight) * left_gain);
            right = (int32_t)(((float)noise * noise_weight + (float)music_right * music_weight) * right_gain);
            float step = (2.0f * AUDIO_PI) / ((float)s_audio.sample_rate_hz * period);
            s_audio.assist_pan_phase += step;
            s_audio.assist_breath_phase += step;
            if (s_audio.assist_pan_phase > 2.0f * AUDIO_PI) {
                s_audio.assist_pan_phase -= 2.0f * AUDIO_PI;
            }
            if (s_audio.assist_breath_phase > 2.0f * AUDIO_PI) {
                s_audio.assist_breath_phase -= 2.0f * AUDIO_PI;
            }
        } else if (mode == AUDIO_MODE_WAKE_MUSIC) {
            static const uint16_t notes_hz[] = {392, 494, 523, 659};
            uint32_t note_index = (s_audio.melody_sample / (s_audio.sample_rate_hz / 2U)) %
                                  (sizeof(notes_hz) / sizeof(notes_hz[0]));
            uint32_t note_step = (uint32_t)(((uint64_t)notes_hz[note_index] * UINT32_MAX) /
                                            s_audio.sample_rate_hz);
            int32_t tone = triangle_wave(s_audio.tone_phase);
            left = tone;
            right = (tone * 3) / 4;
            s_audio.tone_phase += note_step;
            s_audio.melody_sample++;
        } else if (mode == AUDIO_MODE_WAKE_ACK) {
            uint32_t total = s_audio.melody_sample;
            uint32_t first_len = s_audio.sample_rate_hz / 5U;
            uint32_t gap_len = s_audio.sample_rate_hz / 16U;
            uint32_t second_len = s_audio.sample_rate_hz / 4U;
            uint32_t hz = 0;
            uint32_t pos = 0;
            uint32_t len = 1;
            if (total < first_len) {
                hz = 660;
                pos = total;
                len = first_len;
            } else if (total < first_len + gap_len) {
                hz = 0;
            } else if (total < first_len + gap_len + second_len) {
                hz = 880;
                pos = total - first_len - gap_len;
                len = second_len;
            }
            if (hz > 0) {
                uint32_t step = (uint32_t)(((uint64_t)hz * UINT32_MAX) / s_audio.sample_rate_hz);
                int32_t env = triangle_env(pos, len);
                int32_t tone = (sine_wave(s_audio.tone_phase) * env) >> 15;
                left = tone;
                right = tone;
                s_audio.tone_phase += step;
            }
            s_audio.melody_sample++;
        } else if (mode == AUDIO_MODE_SLEEP_MUSIC) {
            sleep_music_preset_t preset = s_audio.sleep_music_preset;
            if (preset < 0 || preset >= (sizeof(SLEEP_MUSIC_MIXES) / sizeof(SLEEP_MUSIC_MIXES[0]))) {
                preset = SLEEP_MUSIC_PRESET_PPM;
            }
            const sleep_music_mix_t *mix = &SLEEP_MUSIC_MIXES[preset];
            render_sleep_music_frame(mix, &left, &right);
        } else if (mode == AUDIO_MODE_TEST_LEFT || mode == AUDIO_MODE_TEST_RIGHT ||
                   mode == AUDIO_MODE_TEST_BOTH) {
            uint32_t left_step = (uint32_t)(((uint64_t)440 * UINT32_MAX) /
                                           s_audio.sample_rate_hz);
            uint32_t right_step = (uint32_t)(((uint64_t)880 * UINT32_MAX) /
                                            s_audio.sample_rate_hz);
            int32_t left_tone = triangle_wave(s_audio.tone_phase);
            int32_t right_tone = triangle_wave(s_audio.breath_phase);
            if (mode == AUDIO_MODE_TEST_LEFT || mode == AUDIO_MODE_TEST_BOTH) {
                left = left_tone;
            }
            if (mode == AUDIO_MODE_TEST_RIGHT || mode == AUDIO_MODE_TEST_BOTH) {
                right = right_tone;
            }
            s_audio.tone_phase += left_step;
            s_audio.breath_phase += right_step;
        }

        if (mode == AUDIO_MODE_SLEEP_ASSIST) {
            frames[i * 2] = clamp_i16((int32_t)((float)left * assist_volume));
            frames[i * 2 + 1] = clamp_i16((int32_t)((float)right * assist_volume));
        } else {
            frames[i * 2] = clamp_i16((left * gain) / INT16_MAX);
            frames[i * 2 + 1] = clamp_i16((right * gain) / INT16_MAX);
        }
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    int16_t *frames = s_render_frames;
    uint8_t fade_in_buffers = 0;

    while (true) {
        audio_mode_t mode;
        uint8_t volume;
        float assist_volume;
        float assist_target;
        float assist_pan_depth;
        bool assist_rhythm;
        uint32_t wav_request_id;
        char wav_path[AUDIO_WAV_PATH_MAX];
        bool wav_loop;
        bool timed_out = false;

        portENTER_CRITICAL(&s_audio.lock);
        if (s_audio.stop_at_us > 0 && esp_timer_get_time() >= s_audio.stop_at_us) {
            s_audio.mode = AUDIO_MODE_SILENT;
            s_audio.stop_at_us = 0;
            s_audio.requested_action = INTERVENTION_ACTION_NONE;
            s_audio.volume_percent = 0;
            s_audio.assist_volume = 0.0f;
            s_audio.wav_loop = false;
            timed_out = true;
        }
        mode = s_audio.mode;
        volume = s_audio.volume_percent;
        assist_volume = s_audio.assist_volume;
        assist_target = s_audio.assist_target_breath_bpm;
        assist_pan_depth = s_audio.assist_pan_depth;
        assist_rhythm = s_audio.assist_rhythm;
        wav_request_id = s_audio.wav_request_id;
        wav_loop = s_audio.wav_loop;
        snprintf(wav_path, sizeof(wav_path), "%s", s_audio.wav_path);
        portEXIT_CRITICAL(&s_audio.lock);

        if (timed_out) {
            ESP_LOGI(TAG, "intervention duration reached; output muted");
        }

        if (mode == AUDIO_MODE_SILENT) {
            if (s_audio.wav_file) {
                close_wav_file();
            }
            audio_output_enable(false);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        if (!s_audio.output_enabled) {
            /* Prime the shared I2S TX path with silence while the codec is
             * muted and the external amplifier is physically disconnected.
             * This prevents stale DMA samples from becoming a power-on pop. */
            memset(frames, 0, AUDIO_FRAME_COUNT * 2 * sizeof(int16_t));
            int prime_err = esp_codec_dev_write(s_audio.speaker, frames,
                                                AUDIO_FRAME_COUNT * 2 * sizeof(int16_t));
            if (prime_err != ESP_CODEC_DEV_OK) {
                portENTER_CRITICAL(&s_audio.lock);
                s_audio.write_errors++;
                set_audio_result_locked((esp_err_t)prime_err, "codec_prime_failed");
                portEXIT_CRITICAL(&s_audio.lock);
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            audio_output_enable(true);
            fade_in_buffers = 8;
        }

        if (mode == AUDIO_MODE_WAV_FILE) {
            if (s_audio.wav_open_request_id != wav_request_id) {
                esp_err_t err = open_wav_request(wav_path, wav_request_id);
                if (err != ESP_OK) {
                    portENTER_CRITICAL(&s_audio.lock);
                    if (s_audio.wav_request_id == wav_request_id) {
                        s_audio.mode = AUDIO_MODE_SILENT;
                        s_audio.stop_at_us = 0;
                        s_audio.volume_percent = 0;
                    }
                    portEXIT_CRITICAL(&s_audio.lock);
                }
            }
            bool eof = fill_wav_frames(frames, AUDIO_FRAME_COUNT, volume);
            if (eof) {
                close_wav_file();
                portENTER_CRITICAL(&s_audio.lock);
                if (s_audio.mode == AUDIO_MODE_WAV_FILE && s_audio.wav_request_id == wav_request_id) {
                    if (wav_loop && s_audio.stop_at_us > esp_timer_get_time()) {
                        s_audio.wav_open_request_id = 0;
                    } else {
                        s_audio.mode = AUDIO_MODE_SILENT;
                        s_audio.stop_at_us = 0;
                        s_audio.volume_percent = 0;
                        s_audio.wav_loop = false;
                    }
                }
                portEXIT_CRITICAL(&s_audio.lock);
                ESP_LOGI(TAG, "%s", wav_loop ? "WAV loop restart" : "WAV playback finished");
            }
        } else {
            if (s_audio.wav_file) {
                close_wav_file();
            }
            if (ensure_codec_sample_rate(s_audio.base_sample_rate_hz) != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(20));
                continue;
            }
            fill_audio_frames(frames, AUDIO_FRAME_COUNT, mode, volume,
                              assist_volume, assist_target, assist_pan_depth, assist_rhythm);
        }
        if (fade_in_buffers > 0) {
            uint8_t step = (uint8_t)(9U - fade_in_buffers);
            for (size_t i = 0; i < AUDIO_FRAME_COUNT * 2; ++i) {
                frames[i] = (int16_t)(((int32_t)frames[i] * step) / 8);
            }
            fade_in_buffers--;
        }
        int write_err = esp_codec_dev_write(s_audio.speaker, frames,
                                            AUDIO_FRAME_COUNT * 2 * sizeof(int16_t));
        if (write_err != ESP_CODEC_DEV_OK) {
            portENTER_CRITICAL(&s_audio.lock);
            s_audio.write_errors++;
            set_audio_result_locked((esp_err_t)write_err, "codec_write_failed");
            portEXIT_CRITICAL(&s_audio.lock);
            ESP_LOGW(TAG, "codec write failed: %d", write_err);
            vTaskDelay(pdMS_TO_TICKS(20));
        } else {
            portENTER_CRITICAL(&s_audio.lock);
            s_audio.write_calls++;
            portEXIT_CRITICAL(&s_audio.lock);
            vTaskDelay(1);
        }
    }
}

esp_err_t stereo_audio_init(const stereo_audio_config_t *config)
{
    if (!config || config->sample_rate_hz == 0) {
        set_audio_result(ESP_ERR_INVALID_ARG, "invalid_config");
        ESP_LOGE(TAG, "invalid audio config");
        return ESP_ERR_INVALID_ARG;
    }
    if (s_audio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    (void)config->i2s_port;
    (void)config->bclk_gpio;
    (void)config->ws_gpio;
    (void)config->dout_gpio;

    (void)gpio_reset_pin(BSP_POWER_AMP_IO);
    (void)gpio_set_direction(BSP_POWER_AMP_IO, GPIO_MODE_OUTPUT);
    (void)gpio_set_level(BSP_POWER_AMP_IO, 0);
    s_audio.speaker = bsp_audio_codec_speaker_init();
    if (!s_audio.speaker) {
        set_audio_result(ESP_ERR_NOT_FOUND, "speaker_codec_init_failed");
        ESP_LOGE(TAG, "speaker codec init failed");
        return ESP_ERR_NOT_FOUND;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = config->sample_rate_hz,
        .channel = 2,
        .bits_per_sample = 16,
    };
    int codec_err = esp_codec_dev_open(s_audio.speaker, &fs);
    if (codec_err != ESP_CODEC_DEV_OK) {
        set_audio_result((esp_err_t)codec_err, "speaker_open_failed");
        ESP_LOGE(TAG, "speaker open failed: %d", codec_err);
        s_audio.speaker = NULL;
        return ESP_FAIL;
    }
    s_audio.codec_open = true;
    codec_err = esp_codec_dev_set_out_vol(s_audio.speaker, AUDIO_CODEC_OUTPUT_VOLUME);
    if (codec_err != ESP_CODEC_DEV_OK) {
        set_audio_result((esp_err_t)codec_err, "speaker_volume_failed");
        ESP_LOGW(TAG, "speaker volume set failed: %d", codec_err);
    }
    (void)esp_codec_dev_set_out_mute(s_audio.speaker, true);
    s_audio.output_enabled = false;

    s_audio.mode = AUDIO_MODE_SILENT;
    s_audio.requested_action = INTERVENTION_ACTION_NONE;
    s_audio.sample_rate_hz = config->sample_rate_hz;
    s_audio.base_sample_rate_hz = config->sample_rate_hz;
    s_audio.codec_sample_rate_hz = config->sample_rate_hz;
    s_audio.noise_state = 0x13579BDFU;
    init_sine_table();
    s_audio.assist_target_breath_bpm = 7.0f;
    s_audio.assist_pan_depth = 0.05f;
    s_audio.initialized = true;
    set_audio_result(ESP_OK, "initialized");

    TaskHandle_t created = xTaskCreateStaticPinnedToCore(audio_task, "stereo_audio",
                                                         AUDIO_TASK_STACK_SIZE, NULL,
                                                         AUDIO_TASK_PRIORITY,
                                                         s_audio_task_stack,
                                                         &s_audio_task_tcb,
                                                         tskNO_AFFINITY);
    if (!created) {
        s_audio.initialized = false;
        s_audio.speaker = NULL;
        set_audio_result(ESP_ERR_NO_MEM, "audio_task_create_failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ES8311 speaker ready: rate=%lu codec_vol=%u",
             (unsigned long)config->sample_rate_hz, AUDIO_CODEC_OUTPUT_VOLUME);
    return ESP_OK;
}

void stereo_audio_get_status(stereo_audio_status_t *status)
{
    if (!status) {
        return;
    }
    portENTER_CRITICAL(&s_audio.lock);
    status->initialized = s_audio.initialized;
    status->mode = (int)s_audio.mode;
    status->volume_percent = s_audio.volume_percent;
    status->sample_rate_hz = s_audio.sample_rate_hz;
    status->wav_request_id = s_audio.wav_request_id;
    status->wav_open_request_id = s_audio.wav_open_request_id;
    status->wav_data_remaining = s_audio.wav_data_remaining;
    status->write_calls = s_audio.write_calls;
    status->write_errors = s_audio.write_errors;
    status->last_error = s_audio.last_error;
    snprintf(status->wav_path, sizeof(status->wav_path), "%s", s_audio.wav_path);
    snprintf(status->last_result, sizeof(status->last_result), "%s", s_audio.last_result);
    portEXIT_CRITICAL(&s_audio.lock);
}

static void set_audio_test_mode(audio_mode_t mode, uint8_t volume_percent, const char *label)
{
    portENTER_CRITICAL(&s_audio.lock);
    s_audio.mode = mode;
    s_audio.requested_action = INTERVENTION_ACTION_NONE;
    s_audio.volume_percent = volume_percent;
    s_audio.assist_volume = 0.0f;
    s_audio.stop_at_us = 0;
    s_audio.tone_phase = 0;
    s_audio.breath_phase = 0;
    s_audio.ambient_phase = 0;
    s_audio.bird_phase = 0;
    s_audio.pad_phase_a = 0;
    s_audio.pad_phase_b = 0;
    s_audio.pad_phase_c = 0;
    s_audio.piano_phase = 0;
    s_audio.noise_lp_fast = 0;
    s_audio.noise_lp_slow = 0;
    s_audio.noise_lp_deep = 0;
    s_audio.melody_sample = 0;
    set_audio_result_locked(ESP_OK, label);
    portEXIT_CRITICAL(&s_audio.lock);
    ESP_LOGI(TAG, "self-test %s volume=%u", label, volume_percent);
}

esp_err_t stereo_audio_self_test(void)
{
    if (!s_audio.initialized) {
        ESP_LOGW(TAG, "self-test skipped: audio not ready");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "self-test: ping-pong LEFT 440Hz / RIGHT 880Hz, then stereo volume steps");
    for (size_t i = 0; i < 4; ++i) {
        set_audio_test_mode(AUDIO_MODE_TEST_LEFT, 55, "PING_LEFT_440Hz");
        vTaskDelay(pdMS_TO_TICKS(650));
        set_audio_test_mode(AUDIO_MODE_SILENT, 0, "gap");
        vTaskDelay(pdMS_TO_TICKS(160));

        set_audio_test_mode(AUDIO_MODE_TEST_RIGHT, 55, "PONG_RIGHT_880Hz");
        vTaskDelay(pdMS_TO_TICKS(650));
        set_audio_test_mode(AUDIO_MODE_SILENT, 0, "gap");
        vTaskDelay(pdMS_TO_TICKS(220));
    }

    const uint8_t levels[] = {12, 25, 40, 55, 30};
    for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); ++i) {
        set_audio_test_mode(AUDIO_MODE_TEST_BOTH, levels[i], "STEREO_440L_880R");
        vTaskDelay(pdMS_TO_TICKS(650));
    }

    set_audio_test_mode(AUDIO_MODE_SILENT, 0, "silent");
    return ESP_OK;
}

esp_err_t stereo_audio_sleep_assist_set(float volume, float target_breath_bpm, float pan_depth, bool rhythm_enabled)
{
    if (!s_audio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (volume < 0.0f) {
        volume = 0.0f;
    }
    if (volume > 0.30f) {
        volume = 0.30f;
    }
    if (target_breath_bpm < 4.0f) {
        target_breath_bpm = 4.0f;
    }
    if (target_breath_bpm > 20.0f) {
        target_breath_bpm = 20.0f;
    }
    if (pan_depth < 0.0f) {
        pan_depth = 0.0f;
    }
    if (pan_depth > 0.10f) {
        pan_depth = 0.10f;
    }

    portENTER_CRITICAL(&s_audio.lock);
    s_audio.mode = volume > 0.0001f ? AUDIO_MODE_SLEEP_ASSIST : AUDIO_MODE_SILENT;
    s_audio.requested_action = INTERVENTION_ACTION_NONE;
    s_audio.volume_percent = 0;
    s_audio.stop_at_us = 0;
    s_audio.assist_volume = volume;
    s_audio.assist_target_breath_bpm = target_breath_bpm;
    s_audio.assist_pan_depth = pan_depth;
    s_audio.assist_rhythm = rhythm_enabled;
    portEXIT_CRITICAL(&s_audio.lock);
    return ESP_OK;
}

esp_err_t stereo_audio_set_sleep_music_preset(const char *preset)
{
    sleep_music_preset_t value = SLEEP_MUSIC_PRESET_PPM;
    if (preset && strcmp(preset, "ocean") == 0) {
        value = SLEEP_MUSIC_PRESET_OCEAN;
    } else if (preset && strcmp(preset, "forest") == 0) {
        value = SLEEP_MUSIC_PRESET_FOREST;
    } else if (preset && strcmp(preset, "rain") == 0) {
        value = SLEEP_MUSIC_PRESET_RAIN;
    } else if (preset && strcmp(preset, "zen") == 0) {
        value = SLEEP_MUSIC_PRESET_ZEN;
    } else if (preset && strcmp(preset, "empty") == 0) {
        value = SLEEP_MUSIC_PRESET_EMPTY;
    }

    portENTER_CRITICAL(&s_audio.lock);
    s_audio.sleep_music_preset = value;
    portEXIT_CRITICAL(&s_audio.lock);
    ESP_LOGI(TAG, "sleep music preset=%s", preset ? preset : "ppm");
    return ESP_OK;
}

static esp_err_t stereo_audio_play_mode(audio_mode_t mode, uint8_t volume_percent, uint16_t duration_sec)
{
    if (!s_audio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (volume_percent > 100) {
        volume_percent = 100;
    }
    if (duration_sec == 0) {
        duration_sec = 60;
    }
    if (duration_sec > 900) {
        duration_sec = 900;
    }

    portENTER_CRITICAL(&s_audio.lock);
    s_audio.mode = mode;
    s_audio.requested_action = INTERVENTION_ACTION_NONE;
    s_audio.volume_percent = volume_percent;
    s_audio.assist_volume = 0.0f;
    s_audio.wav_loop = false;
    s_audio.stop_at_us = esp_timer_get_time() + (int64_t)duration_sec * 1000000LL;
    s_audio.tone_phase = 0;
    s_audio.breath_phase = 0;
    s_audio.ambient_phase = 0;
    s_audio.bird_phase = 0;
    s_audio.pad_phase_a = 0;
    s_audio.pad_phase_b = 0;
    s_audio.pad_phase_c = 0;
    s_audio.piano_phase = 0;
    s_audio.noise_lp_fast = 0;
    s_audio.noise_lp_slow = 0;
    s_audio.noise_lp_deep = 0;
    s_audio.melody_sample = 0;
    set_audio_result_locked(ESP_OK, "mode_playing");
    portEXIT_CRITICAL(&s_audio.lock);
    ESP_LOGI(TAG, "manual mode=%d volume=%u duration=%us", mode, volume_percent, duration_sec);
    return ESP_OK;
}

esp_err_t stereo_audio_play_music(uint8_t volume_percent, uint16_t duration_sec)
{
    return stereo_audio_play_mode(AUDIO_MODE_SLEEP_MUSIC, volume_percent, duration_sec);
}

esp_err_t stereo_audio_play_pink_noise(uint8_t volume_percent, uint16_t duration_sec)
{
    return stereo_audio_play_mode(AUDIO_MODE_PINK_NOISE, volume_percent, duration_sec);
}

esp_err_t stereo_audio_play_breathing(uint8_t volume_percent, uint16_t duration_sec)
{
    return stereo_audio_play_mode(AUDIO_MODE_STEREO_BREATHING, volume_percent, duration_sec);
}

esp_err_t stereo_audio_play_wake_ack(uint8_t volume_percent, uint16_t duration_sec)
{
    if (duration_sec == 0 || duration_sec > 3) {
        duration_sec = 1;
    }
    return stereo_audio_play_mode(AUDIO_MODE_WAKE_ACK, volume_percent, duration_sec);
}

static esp_err_t stereo_audio_play_wav_internal(const char *path, uint8_t volume_percent,
                                                uint16_t duration_sec, bool loop)
{
    if (!s_audio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!path || !path[0] || strlen(path) >= AUDIO_WAV_PATH_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (volume_percent > 100) {
        volume_percent = 100;
    }
    if (duration_sec == 0) {
        duration_sec = 300;
    }
    if (duration_sec > 1800) {
        duration_sec = 1800;
    }

    wav_info_t info = {0};
    esp_err_t err = wav_probe_path(path, &info);
    if (err != ESP_OK) {
        set_audio_result(err, "wav_probe_failed");
        return err;
    }
    if (info.sample_rate_hz < 8000 || info.sample_rate_hz > 48000) {
        set_audio_result(ESP_ERR_NOT_SUPPORTED, "wav_probe_rate_unsupported");
        return ESP_ERR_NOT_SUPPORTED;
    }
    portENTER_CRITICAL(&s_audio.lock);
    s_audio.mode = AUDIO_MODE_WAV_FILE;
    s_audio.requested_action = INTERVENTION_ACTION_NONE;
    s_audio.volume_percent = volume_percent;
    s_audio.assist_volume = 0.0f;
    s_audio.stop_at_us = esp_timer_get_time() + (int64_t)duration_sec * 1000000LL;
    s_audio.tone_phase = 0;
    s_audio.breath_phase = 0;
    s_audio.ambient_phase = 0;
    s_audio.bird_phase = 0;
    s_audio.pad_phase_a = 0;
    s_audio.pad_phase_b = 0;
    s_audio.pad_phase_c = 0;
    s_audio.piano_phase = 0;
    s_audio.melody_sample = 0;
    snprintf(s_audio.wav_path, sizeof(s_audio.wav_path), "%s", path);
    s_audio.wav_loop = loop;
    s_audio.wav_request_id++;
    if (s_audio.wav_request_id == 0) {
        s_audio.wav_request_id = 1;
    }
    set_audio_result_locked(ESP_OK, "wav_requested");
    portEXIT_CRITICAL(&s_audio.lock);
    ESP_LOGI(TAG, "manual WAV path=%s volume=%u duration=%us loop=%d",
             path, volume_percent, duration_sec, loop);
    return ESP_OK;
}

esp_err_t stereo_audio_play_wav(const char *path, uint8_t volume_percent, uint16_t duration_sec)
{
    return stereo_audio_play_wav_internal(path, volume_percent, duration_sec, false);
}

esp_err_t stereo_audio_play_wav_loop(const char *path, uint8_t volume_percent, uint16_t duration_sec)
{
    return stereo_audio_play_wav_internal(path, volume_percent, duration_sec, true);
}

esp_err_t stereo_audio_set_current_volume(uint8_t volume_percent)
{
    if (!s_audio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (volume_percent > 100) {
        volume_percent = 100;
    }
    portENTER_CRITICAL(&s_audio.lock);
    s_audio.volume_percent = volume_percent;
    if (s_audio.assist_volume > 0.0001f) {
        s_audio.assist_volume = (float)volume_percent / 100.0f;
        if (s_audio.assist_volume > 0.30f) {
            s_audio.assist_volume = 0.30f;
        }
    }
    set_audio_result_locked(ESP_OK, "volume_set");
    portEXIT_CRITICAL(&s_audio.lock);
    if (volume_percent == 0) {
        audio_output_enable(false);
    } else {
        audio_output_enable(true);
    }
    ESP_LOGI(TAG, "current volume=%u", volume_percent);
    return ESP_OK;
}

esp_err_t stereo_audio_mute(void)
{
    if (!s_audio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_audio.lock);
    bool already_muted = s_audio.mode == AUDIO_MODE_SILENT &&
                         s_audio.volume_percent == 0 &&
                         s_audio.assist_volume <= 0.0001f &&
                         s_audio.wav_path[0] == '\0';
    if (already_muted) {
        portEXIT_CRITICAL(&s_audio.lock);
        audio_output_enable(false);
        return ESP_OK;
    }
    s_audio.mode = AUDIO_MODE_SILENT;
    s_audio.requested_action = INTERVENTION_ACTION_NONE;
    s_audio.volume_percent = 0;
    s_audio.stop_at_us = 0;
    s_audio.assist_volume = 0.0f;
    s_audio.assist_rhythm = false;
    s_audio.wav_path[0] = '\0';
    s_audio.wav_loop = false;
    s_audio.wav_request_id++;
    set_audio_result_locked(ESP_OK, "muted");
    portEXIT_CRITICAL(&s_audio.lock);
    audio_output_enable(false);
    return ESP_OK;
}
esp_err_t stereo_audio_apply_decision(const intervention_decision_t *decision)
{
    ESP_RETURN_ON_FALSE(decision, ESP_ERR_INVALID_ARG, TAG, "decision is null");
    if (!s_audio.initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    audio_mode_t requested_mode = AUDIO_MODE_SILENT;
    bool explicit_stop = strcmp(decision->skill_name, "voice.stop") == 0 ||
                         strcmp(decision->skill_name, "voice.wake_complete") == 0;
    if (decision->action == INTERVENTION_ACTION_NONE && !explicit_stop) {
        return ESP_OK;
    }

    if (decision->action == INTERVENTION_ACTION_PINK_NOISE ||
        decision->action == INTERVENTION_ACTION_REDUCE_AROUSAL) {
        requested_mode = AUDIO_MODE_PINK_NOISE;
    } else if (decision->action == INTERVENTION_ACTION_STEREO_BREATHING) {
        requested_mode = AUDIO_MODE_STEREO_BREATHING;
    } else if (decision->action == INTERVENTION_ACTION_WAKE_MUSIC) {
        requested_mode = AUDIO_MODE_WAKE_MUSIC;
    }

    int64_t stop_at_us = (requested_mode != AUDIO_MODE_SILENT && decision->duration_sec > 0)
                             ? esp_timer_get_time() + (int64_t)decision->duration_sec * 1000000LL
                             : 0;

    portENTER_CRITICAL(&s_audio.lock);
    bool same_output = decision->action == s_audio.requested_action &&
                       decision->volume_percent == s_audio.volume_percent &&
                       requested_mode == s_audio.mode;

    if (decision->action != s_audio.requested_action || requested_mode != s_audio.mode) {
        s_audio.tone_phase = 0;
        s_audio.breath_phase = 0;
        s_audio.ambient_phase = 0;
        s_audio.bird_phase = 0;
        s_audio.melody_sample = 0;
    }
    s_audio.requested_action = decision->action;
    s_audio.mode = requested_mode;
    s_audio.volume_percent = decision->volume_percent;
    s_audio.assist_volume = 0.0f;
    s_audio.stop_at_us = stop_at_us;
    portEXIT_CRITICAL(&s_audio.lock);

    if (!same_output) {
        ESP_LOGI(TAG, "mode=%d volume=%u duration=%us", requested_mode,
                 decision->volume_percent, decision->duration_sec);
    }
    return ESP_OK;
}
