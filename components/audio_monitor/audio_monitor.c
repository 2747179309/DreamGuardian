#include "audio_monitor.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define AUDIO_MONITOR_DEFAULT_RATE 16000U
#define AUDIO_MONITOR_FRAME_MS 80U
#define AUDIO_MONITOR_MAX_SAMPLES 1280U
#define AUDIO_MONITOR_TASK_STACK 4096
#define AUDIO_MONITOR_TASK_PRIORITY 3
#define AUDIO_MONITOR_MIN_DBFS (-90.0f)

/* Snore pulse segmentation.  A high start threshold rejects the stationary
 * room noise, while a lower end threshold keeps the two lobes of one snore
 * together.  The short refractory period separates successive breaths
 * without the former 1.8 s blind window. */
#define SNORE_START_DBFS (-40.0f)
#define SNORE_START_ABOVE_FLOOR_DB 8.0f
#define SNORE_END_DBFS (-56.0f)
#define SNORE_END_ABOVE_FLOOR_DB 4.0f
#define SNORE_ZCR_MIN 0.008f
#define SNORE_ZCR_MAX 0.500f
#define SNORE_MIN_DURATION_US 160000LL
#define SNORE_MAX_DURATION_US 4000000LL
#define SNORE_END_QUIET_US 240000LL
#define SNORE_REFRACTORY_US 320000LL
#define SNORE_CONFIRMATION_HOLDOFF_US 15000000LL
#define SNORE_SAME_EVENT_MERGE_US 2200000LL
#define SNORE_CALIBRATION_US 800000LL
#define SNORE_MIN_SIGNAL_FRAMES 3U
#define SNORE_MIN_BAND_FRAMES 2U
#define SNORE_MIN_BAND_PERCENT 40U
#define SNORE_SPEECH_VETO_MIN_PERCENT 65U
#define SNORE_SPEECH_VETO_MAX_AVG_ZCR 0.160f
#define SNORE_SEQUENCE_MIN_PULSES 3U
#define SNORE_SEQUENCE_MIN_INTERVAL_US 650000LL
#define SNORE_SEQUENCE_MAX_INTERVAL_US 10000000LL
#define SNORE_CONTEXT_MAX_AGE_US 2000000LL
#define SNORE_PRESENCE_GRACE_US 1500000LL
#define SNORE_BREATH_GRACE_US 6000000LL

static const char *TAG = "audio_monitor";

typedef struct {
    audio_monitor_status_t status;
    audio_monitor_config_t cfg;
    esp_codec_dev_handle_t mic;
    bool started;
    bool shared_stream_initialized;
    bool shared_stream_active;
    bool playback_active;
    bool person_present;
    bool breath_valid;
    bool snore_candidate_active;
    bool snore_sequence_confirmed;
    bool prev_voice_activity;
    bool wake_pending;
    bool phrase_active;
    uint8_t wake_burst_count;
    int64_t first_wake_burst_us;
    int64_t phrase_started_us;
    int64_t phrase_last_active_us;
    int64_t wake_listening_until_us;
    int64_t last_wake_event_us;
    int64_t snore_candidate_started_us;
    int64_t snore_candidate_last_signal_us;
    int64_t snore_refractory_until_us;
    int64_t snore_analysis_started_us;
    int64_t snore_context_update_us;
    int64_t snore_presence_last_seen_us;
    int64_t snore_breath_last_seen_us;
    int64_t snore_last_pulse_started_us;
    int64_t quiet_started_us;
    int64_t last_voice_us;
    int64_t last_shared_status_log_us;
    uint16_t phrase_frames;
    uint16_t snore_signal_frames;
    uint16_t snore_band_frames;
    uint16_t snore_afe_speech_frames;
    uint16_t snore_sequence_pulses;
    uint16_t snore_last_pulse_duration_ms;
    float phrase_peak;
    float phrase_above_floor_db;
    float snore_peak_dbfs;
    float snore_peak_above_floor_db;
    float snore_peak_zcr;
    float snore_zcr_sum;
    float snore_last_pulse_peak_dbfs;
    float snore_last_pulse_zcr;
    float breath_bpm;
    portMUX_TYPE lock;
} audio_monitor_context_t;

static audio_monitor_context_t s_mon = {
    .lock = portMUX_INITIALIZER_UNLOCKED,
};

void audio_monitor_get_default_config(audio_monitor_config_t *config)
{
    if (!config) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->enabled = true;
    config->sample_rate_hz = AUDIO_MONITOR_DEFAULT_RATE;
    config->frame_ms = AUDIO_MONITOR_FRAME_MS;
    config->input_gain_db = 24.0f;
}

static float clampf_local(float value, float lo, float hi)
{
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

static float dbfs_from_rms(float rms)
{
    if (rms < 1.0f) {
        return AUDIO_MONITOR_MIN_DBFS;
    }
    float db = 20.0f * log10f(rms / 32768.0f);
    return clampf_local(db, AUDIO_MONITOR_MIN_DBFS, 0.0f);
}

static void reset_snore_candidate_locked(void)
{
    s_mon.snore_candidate_active = false;
    s_mon.snore_candidate_started_us = 0;
    s_mon.snore_candidate_last_signal_us = 0;
    s_mon.snore_signal_frames = 0;
    s_mon.snore_band_frames = 0;
    s_mon.snore_afe_speech_frames = 0;
    s_mon.snore_peak_dbfs = AUDIO_MONITOR_MIN_DBFS;
    s_mon.snore_peak_above_floor_db = AUDIO_MONITOR_MIN_DBFS;
    s_mon.snore_peak_zcr = 0.0f;
    s_mon.snore_zcr_sum = 0.0f;
}

static void reset_snore_sequence_locked(void)
{
    s_mon.snore_sequence_confirmed = false;
    s_mon.snore_sequence_pulses = 0;
    s_mon.snore_last_pulse_started_us = 0;
    s_mon.snore_last_pulse_duration_ms = 0;
    s_mon.snore_last_pulse_peak_dbfs = AUDIO_MONITOR_MIN_DBFS;
    s_mon.snore_last_pulse_zcr = 0.0f;
}

static void update_status(float rms, float peak, float zcr, int64_t now_us,
                          bool allow_wake_detection, bool allow_quiet_candidates,
                          bool afe_speech)
{
    bool snore_event_new = false;
    bool snore_event_rejected = false;
    uint32_t snore_event_number = 0;
    uint32_t snore_rejected_number = 0;
    uint32_t snore_duration_ms = 0;
    float snore_event_peak_dbfs = AUDIO_MONITOR_MIN_DBFS;
    float snore_event_floor_dbfs = AUDIO_MONITOR_MIN_DBFS;
    float snore_event_above_floor_db = AUDIO_MONITOR_MIN_DBFS;
    float snore_event_zcr = 0.0f;
    uint32_t snore_event_band_percent = 0;
    uint32_t snore_event_speech_percent = 0;
    float snore_event_avg_zcr = 0.0f;
    bool snore_event_speech_veto = false;
    uint16_t snore_pending_pulses = 0;
    uint16_t snore_confirmed_pulses = 0;
    uint32_t snore_interval_ms = 0;
    portENTER_CRITICAL(&s_mon.lock);
    audio_monitor_status_t *st = &s_mon.status;
    float dbfs = dbfs_from_rms(rms);
    st->frames++;
    st->rms = st->rms <= 0.01f ? rms : st->rms * 0.86f + rms * 0.14f;
    st->peak = peak;
    st->level_dbfs = st->level_dbfs <= AUDIO_MONITOR_MIN_DBFS + 0.1f
                         ? dbfs
                         : st->level_dbfs * 0.82f + dbfs * 0.18f;
    bool calibrating = s_mon.snore_analysis_started_us > 0 &&
                       now_us - s_mon.snore_analysis_started_us < SNORE_CALIBRATION_US;
    if (st->noise_floor_dbfs <= AUDIO_MONITOR_MIN_DBFS + 0.1f) {
        st->noise_floor_dbfs = dbfs;
    } else if (calibrating) {
        /* Quickly establish the room floor immediately after deep lock. */
        st->noise_floor_dbfs = st->noise_floor_dbfs * 0.92f + dbfs * 0.08f;
    } else if (!s_mon.snore_candidate_active && dbfs < st->noise_floor_dbfs + 6.0f) {
        st->noise_floor_dbfs = st->noise_floor_dbfs * 0.995f + dbfs * 0.005f;
    }

    float instant_above_floor = dbfs - st->noise_floor_dbfs;
    st->instant_above_floor_db = instant_above_floor;
    bool speech_frame = ((instant_above_floor > 2.0f && dbfs > -68.0f) || peak > 150.0f) &&
                        zcr > 0.006f && zcr < 0.22f;
    st->voice_activity = speech_frame;
    if (st->voice_activity) {
        s_mon.last_voice_us = now_us;
    }

    bool snore_band_like = zcr >= SNORE_ZCR_MIN && zcr <= SNORE_ZCR_MAX;
    /* Capture the complete acoustic pulse as soon as its level is credible.
     * A snore attack can have a brief high-ZCR edge, so applying the frequency
     * gate to only this first frame makes otherwise valid snores disappear.
     * Frequency consistency is evaluated over the completed event below. */
    bool snore_start_like = !calibrating && dbfs >= SNORE_START_DBFS &&
                            instant_above_floor >= SNORE_START_ABOVE_FLOOR_DB;
    bool snore_signal_like = dbfs >= SNORE_END_DBFS &&
                             instant_above_floor >= SNORE_END_ABOVE_FLOOR_DB;

    if (!s_mon.snore_candidate_active && snore_start_like &&
        now_us >= s_mon.snore_refractory_until_us) {
        s_mon.snore_candidate_active = true;
        s_mon.snore_candidate_started_us = now_us;
        s_mon.snore_candidate_last_signal_us = now_us;
        s_mon.snore_signal_frames = 1;
        s_mon.snore_band_frames = snore_band_like ? 1 : 0;
        s_mon.snore_afe_speech_frames = afe_speech ? 1 : 0;
        s_mon.snore_peak_dbfs = dbfs;
        s_mon.snore_peak_above_floor_db = instant_above_floor;
        s_mon.snore_peak_zcr = zcr;
        s_mon.snore_zcr_sum = zcr;
    } else if (s_mon.snore_candidate_active) {
        if (snore_signal_like) {
            s_mon.snore_candidate_last_signal_us = now_us;
            if (s_mon.snore_signal_frames < UINT16_MAX) {
                s_mon.snore_signal_frames++;
            }
            if (snore_band_like && s_mon.snore_band_frames < UINT16_MAX) {
                s_mon.snore_band_frames++;
            }
            if (afe_speech && s_mon.snore_afe_speech_frames < UINT16_MAX) {
                s_mon.snore_afe_speech_frames++;
            }
            s_mon.snore_zcr_sum += zcr;
            if (dbfs > s_mon.snore_peak_dbfs) {
                s_mon.snore_peak_dbfs = dbfs;
                s_mon.snore_peak_above_floor_db = instant_above_floor;
                s_mon.snore_peak_zcr = zcr;
            }
        }

        int64_t candidate_span_us = now_us - s_mon.snore_candidate_started_us;
        bool ended_by_quiet = now_us - s_mon.snore_candidate_last_signal_us >=
                              SNORE_END_QUIET_US;
        bool ended_by_limit = candidate_span_us >= SNORE_MAX_DURATION_US;
        if (ended_by_quiet || ended_by_limit) {
            int64_t signal_duration_us = s_mon.snore_candidate_last_signal_us -
                                         s_mon.snore_candidate_started_us +
                                         (int64_t)(s_mon.cfg.frame_ms ? s_mon.cfg.frame_ms :
                                                   AUDIO_MONITOR_FRAME_MS) * 1000LL;
            uint32_t band_percent = s_mon.snore_signal_frames > 0
                                        ? ((uint32_t)s_mon.snore_band_frames * 100U) /
                                              (uint32_t)s_mon.snore_signal_frames
                                        : 0U;
            snore_event_band_percent = band_percent;
            uint32_t speech_percent = s_mon.snore_signal_frames > 0
                                          ? ((uint32_t)s_mon.snore_afe_speech_frames * 100U) /
                                                (uint32_t)s_mon.snore_signal_frames
                                          : 0U;
            float avg_zcr = s_mon.snore_signal_frames > 0
                                ? s_mon.snore_zcr_sum / (float)s_mon.snore_signal_frames
                                : 0.0f;
            bool human_speech_like = speech_percent >= SNORE_SPEECH_VETO_MIN_PERCENT &&
                                     avg_zcr <= SNORE_SPEECH_VETO_MAX_AVG_ZCR;
            snore_event_speech_percent = speech_percent;
            snore_event_avg_zcr = avg_zcr;
            snore_event_speech_veto = human_speech_like;
            bool pulse_shape_ok = !ended_by_limit &&
                                  !human_speech_like &&
                                  signal_duration_us >= SNORE_MIN_DURATION_US &&
                                  s_mon.snore_signal_frames >= SNORE_MIN_SIGNAL_FRAMES &&
                                  s_mon.snore_band_frames >= SNORE_MIN_BAND_FRAMES &&
                                  band_percent >= SNORE_MIN_BAND_PERCENT &&
                                  s_mon.snore_peak_dbfs >= SNORE_START_DBFS &&
                                  s_mon.snore_peak_above_floor_db >=
                                      SNORE_START_ABOVE_FLOOR_DB;

            snore_duration_ms = (uint32_t)(signal_duration_us / 1000LL);
            snore_event_peak_dbfs = s_mon.snore_peak_dbfs;
            snore_event_floor_dbfs = st->noise_floor_dbfs;
            snore_event_above_floor_db = s_mon.snore_peak_above_floor_db;
            snore_event_zcr = s_mon.snore_peak_zcr;
            st->snore_last_duration_ms = (uint16_t)(snore_duration_ms > UINT16_MAX
                                                        ? UINT16_MAX
                                                        : snore_duration_ms);
            st->snore_last_peak_dbfs = snore_event_peak_dbfs;
            st->snore_last_peak_above_floor_db = snore_event_above_floor_db;
            st->snore_last_zcr = snore_event_zcr;
            if (pulse_shape_ok) {
                int64_t pulse_started_us = s_mon.snore_candidate_started_us;
                int64_t interval_us = s_mon.snore_last_pulse_started_us > 0
                                          ? pulse_started_us - s_mon.snore_last_pulse_started_us
                                          : 0;
                /* One acoustic snore often contains several loud lobes with
                 * short quiet gaps.  Merge those lobes at the event layer;
                 * importantly, do not move the last logical-snore timestamp,
                 * otherwise a train of lobes could extend the merge window. */
                bool duplicate_lobe = s_mon.snore_last_pulse_started_us > 0 &&
                                      interval_us > 0 &&
                                      interval_us < SNORE_SAME_EVENT_MERGE_US;
                if (!duplicate_lobe) {
                    /* Radar validates that a breathing person is present.  Do
                     * not derive a narrow interval from its very noisy
                     * instantaneous BPM value: three replayed or real snores
                     * should form a sequence anywhere in this physiological
                     * spacing window. */
                    int64_t min_interval_us = SNORE_SEQUENCE_MIN_INTERVAL_US;
                    int64_t max_interval_us = SNORE_SEQUENCE_MAX_INTERVAL_US;

                    float duration_ratio = s_mon.snore_last_pulse_duration_ms > 0
                                               ? (float)snore_duration_ms /
                                                     (float)s_mon.snore_last_pulse_duration_ms
                                               : 1.0f;
                    if (duration_ratio < 1.0f) {
                        duration_ratio = 1.0f / duration_ratio;
                    }
                    float peak_delta = fabsf(snore_event_peak_dbfs -
                                             s_mon.snore_last_pulse_peak_dbfs);
                    float zcr_delta = fabsf(snore_event_zcr - s_mon.snore_last_pulse_zcr);
                    bool interval_matches_breath = interval_us >= min_interval_us &&
                                                   interval_us <= max_interval_us;
                    bool pulse_shape_similar = duration_ratio <= 3.0f &&
                                               peak_delta <= 20.0f &&
                                               zcr_delta <= 0.10f;
                    bool continues_sequence = s_mon.snore_sequence_pulses > 0 &&
                                              interval_matches_breath &&
                                              pulse_shape_similar;

                    if (!continues_sequence) {
                        reset_snore_sequence_locked();
                        s_mon.snore_sequence_pulses = 1;
                    } else if (s_mon.snore_sequence_pulses < UINT16_MAX) {
                        s_mon.snore_sequence_pulses++;
                    }

                    s_mon.snore_last_pulse_started_us = pulse_started_us;
                    s_mon.snore_last_pulse_duration_ms =
                        (uint16_t)(snore_duration_ms > UINT16_MAX ? UINT16_MAX : snore_duration_ms);
                    s_mon.snore_last_pulse_peak_dbfs = snore_event_peak_dbfs;
                    s_mon.snore_last_pulse_zcr = snore_event_zcr;
                    snore_pending_pulses = s_mon.snore_sequence_pulses;
                    snore_interval_ms = interval_us > 0 ? (uint32_t)(interval_us / 1000LL) : 0;

                    if (!s_mon.snore_sequence_confirmed &&
                        s_mon.snore_sequence_pulses >= SNORE_SEQUENCE_MIN_PULSES) {
                        s_mon.snore_sequence_confirmed = true;
                        snore_confirmed_pulses = s_mon.snore_sequence_pulses;
                        st->snore_events += snore_confirmed_pulses;
                        snore_event_number = st->snore_events;
                        snore_event_new = true;
                        /* A confirmation must not make later sounds easier to
                         * accept.  Close this sequence immediately so every
                         * future report has to build a fresh three-pulse
                         * rhythm, just like the first one. */
                        reset_snore_sequence_locked();
                    }
                }
            } else {
                st->snore_rejected_events++;
                snore_rejected_number = st->snore_rejected_events;
                snore_event_rejected = true;
            }
            /* Once three matching snores have confirmed an episode, keep
             * its late echo/tail from starting another sequence.  The
             * sequence itself was reset above, so a later episode must
             * always build a fresh three-pulse confirmation. */
            s_mon.snore_refractory_until_us =
                now_us + (snore_event_new &&
                                  snore_confirmed_pulses >= SNORE_SEQUENCE_MIN_PULSES
                              ? SNORE_CONFIRMATION_HOLDOFF_US
                              : SNORE_REFRACTORY_US);
            reset_snore_candidate_locked();
        }
    }
    st->snore_active = s_mon.snore_candidate_active;

    bool very_quiet = st->level_dbfs < st->noise_floor_dbfs + 2.0f && st->level_dbfs < -58.0f;
    if (allow_quiet_candidates && very_quiet) {
        if (s_mon.quiet_started_us == 0) {
            s_mon.quiet_started_us = now_us;
        }
        st->quiet_seconds = (uint32_t)((now_us - s_mon.quiet_started_us) / 1000000LL);
        if (st->quiet_seconds >= 10 && (st->quiet_seconds % 10U) == 0U) {
            st->apnea_candidates++;
            s_mon.quiet_started_us = now_us + 1000000LL;
        }
    } else {
        s_mon.quiet_started_us = 0;
        st->quiet_seconds = 0;
    }

    st->noise_high = st->noise_floor_dbfs > -48.0f || st->level_dbfs > -38.0f;
    st->environment_score = clampf_local((st->level_dbfs + 65.0f) / 35.0f, 0.0f, 1.0f);
    st->volume_gain = st->noise_high ? 1.18f : 1.0f;
    if (st->noise_floor_dbfs < -62.0f && !st->voice_activity) {
        st->volume_gain = 0.85f;
    }
    if (st->snore_active) {
        st->volume_gain = 0.75f;
    }
    st->volume_gain = clampf_local(st->volume_gain, 0.75f, 1.25f);

    bool wake_cooldown_ready = now_us - s_mon.last_wake_event_us > 5000000LL;
    if (allow_wake_detection && speech_frame) {
        if (!s_mon.phrase_active) {
            s_mon.phrase_active = true;
            s_mon.phrase_started_us = now_us;
            s_mon.phrase_frames = 0;
            s_mon.phrase_peak = 0.0f;
            s_mon.phrase_above_floor_db = -90.0f;
        }
        s_mon.phrase_last_active_us = now_us;
        s_mon.phrase_frames++;
        if (peak > s_mon.phrase_peak) {
            s_mon.phrase_peak = peak;
        }
        if (instant_above_floor > s_mon.phrase_above_floor_db) {
            s_mon.phrase_above_floor_db = instant_above_floor;
        }
    } else if (allow_wake_detection && s_mon.phrase_active &&
               now_us - s_mon.phrase_last_active_us > 240000LL) {
        uint32_t phrase_ms = (uint32_t)((s_mon.phrase_last_active_us -
                                         s_mon.phrase_started_us) / 1000LL) +
                             (s_mon.cfg.frame_ms ? s_mon.cfg.frame_ms : AUDIO_MONITOR_FRAME_MS);
        st->phrase_ms = phrase_ms;
        st->phrase_peak = s_mon.phrase_peak;
        st->phrase_above_floor_db = s_mon.phrase_above_floor_db;
        bool phrase_like_wake = phrase_ms >= 350U && phrase_ms <= 2500U &&
                                s_mon.phrase_frames >= 4U &&
                                (s_mon.phrase_peak > 120.0f ||
                                 s_mon.phrase_above_floor_db > 2.5f);
        if (phrase_like_wake && wake_cooldown_ready) {
            st->wake_events++;
            s_mon.wake_pending = true;
            s_mon.wake_listening_until_us = now_us + 8000000LL;
            s_mon.last_wake_event_us = now_us;
        }
        s_mon.phrase_active = false;
        s_mon.phrase_started_us = 0;
        s_mon.phrase_last_active_us = 0;
        s_mon.phrase_frames = 0;
        s_mon.phrase_peak = 0.0f;
        s_mon.phrase_above_floor_db = -90.0f;
    }
    if (!allow_wake_detection && s_mon.phrase_active) {
        s_mon.phrase_active = false;
        s_mon.phrase_started_us = 0;
        s_mon.phrase_last_active_us = 0;
        s_mon.phrase_frames = 0;
        s_mon.phrase_peak = 0.0f;
        s_mon.phrase_above_floor_db = -90.0f;
    }
    s_mon.prev_voice_activity = st->voice_activity;
    st->wake_word_configured = allow_wake_detection;
    st->wake_listening = allow_wake_detection && now_us < s_mon.wake_listening_until_us;
    portEXIT_CRITICAL(&s_mon.lock);

    if (snore_event_new) {
        printf("DG Snore: confirmed total=%u added=%u sequence=%u interval=%ums "
               "duration=%ums peak=%.1f floor=%.1f above=%.1f zcr=%.3f "
               "avg_zcr=%.3f band=%u%% speech=%u%%\r\n",
               (unsigned)snore_event_number, (unsigned)snore_confirmed_pulses,
               (unsigned)snore_pending_pulses,
               (unsigned)snore_interval_ms,
               (unsigned)snore_duration_ms,
               (double)snore_event_peak_dbfs, (double)snore_event_floor_dbfs,
               (double)snore_event_above_floor_db, (double)snore_event_zcr,
               (double)snore_event_avg_zcr, (unsigned)snore_event_band_percent,
               (unsigned)snore_event_speech_percent);
    } else if (snore_event_rejected) {
        printf("DG Snore: rejected event=%u duration=%ums peak=%.1f above=%.1f "
               "zcr=%.3f avg_zcr=%.3f band=%u%% speech=%u%% speech_veto=%d\r\n",
               (unsigned)snore_rejected_number, (unsigned)snore_duration_ms,
               (double)snore_event_peak_dbfs, (double)snore_event_above_floor_db,
               (double)snore_event_zcr, (double)snore_event_avg_zcr,
               (unsigned)snore_event_band_percent,
               (unsigned)snore_event_speech_percent,
               snore_event_speech_veto ? 1 : 0);
    }
}

static void audio_monitor_task(void *arg)
{
    (void)arg;
    static int16_t samples[AUDIO_MONITOR_MAX_SAMPLES];
    uint32_t sample_rate = s_mon.cfg.sample_rate_hz ? s_mon.cfg.sample_rate_hz : AUDIO_MONITOR_DEFAULT_RATE;
    uint32_t frame_ms = s_mon.cfg.frame_ms ? s_mon.cfg.frame_ms : AUDIO_MONITOR_FRAME_MS;
    size_t sample_count = (sample_rate * frame_ms) / 1000U;
    if (sample_count == 0 || sample_count > AUDIO_MONITOR_MAX_SAMPLES) {
        sample_count = AUDIO_MONITOR_MAX_SAMPLES;
    }

    esp_codec_dev_sample_info_t fs = {
        .sample_rate = sample_rate,
        .channel = 1,
        .bits_per_sample = 16,
    };
    int err = esp_codec_dev_open(s_mon.mic, &fs);
    if (err != ESP_CODEC_DEV_OK) {
        portENTER_CRITICAL(&s_mon.lock);
        s_mon.status.running = false;
        s_mon.status.last_error = (esp_err_t)err;
        portEXIT_CRITICAL(&s_mon.lock);
        ESP_LOGW(TAG, "mic open failed err=%d", err);
        vTaskDelete(NULL);
        return;
    }
    (void)esp_codec_dev_set_in_gain(s_mon.mic, s_mon.cfg.input_gain_db);

    portENTER_CRITICAL(&s_mon.lock);
    s_mon.status.running = true;
    s_mon.status.initialized = true;
    s_mon.status.sample_rate_hz = sample_rate;
    s_mon.status.frame_ms = (uint16_t)frame_ms;
    s_mon.status.last_error = ESP_OK;
    portEXIT_CRITICAL(&s_mon.lock);

    while (true) {
        int read_err = esp_codec_dev_read(s_mon.mic, samples, (int)(sample_count * sizeof(int16_t)));
        if (read_err != ESP_CODEC_DEV_OK) {
            portENTER_CRITICAL(&s_mon.lock);
            s_mon.status.read_errors++;
            s_mon.status.last_error = (esp_err_t)read_err;
            portEXIT_CRITICAL(&s_mon.lock);
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        int64_t sum_sq = 0;
        int32_t peak = 0;
        uint32_t zero_cross = 0;
        int16_t prev = samples[0];
        for (size_t i = 0; i < sample_count; ++i) {
            int32_t s = samples[i];
            sum_sq += (int64_t)s * (int64_t)s;
            int32_t abs_s = s < 0 ? -s : s;
            if (abs_s > peak) {
                peak = abs_s;
            }
            if (i > 0 && ((prev < 0 && s >= 0) || (prev >= 0 && s < 0))) {
                zero_cross++;
            }
            prev = (int16_t)s;
        }

        float rms = sqrtf((float)sum_sq / (float)sample_count);
        float zcr = (float)zero_cross / (float)sample_count;
        update_status(rms, (float)peak, zcr, esp_timer_get_time(), true, true, false);
    }
}

esp_err_t audio_monitor_shared_stream_init(uint32_t sample_rate_hz, uint16_t frame_ms)
{
    if (sample_rate_hz == 0 || frame_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_mon.lock);
    if (s_mon.started) {
        portEXIT_CRITICAL(&s_mon.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_mon.shared_stream_initialized) {
        memset(&s_mon.status, 0, sizeof(s_mon.status));
        audio_monitor_get_default_config(&s_mon.cfg);
        s_mon.cfg.sample_rate_hz = sample_rate_hz;
        s_mon.cfg.frame_ms = frame_ms;
        s_mon.status.enabled = true;
        s_mon.status.initialized = true;
        s_mon.status.running = false;
        s_mon.status.sample_rate_hz = sample_rate_hz;
        s_mon.status.frame_ms = frame_ms;
        s_mon.status.level_dbfs = AUDIO_MONITOR_MIN_DBFS;
        s_mon.status.noise_floor_dbfs = AUDIO_MONITOR_MIN_DBFS;
        s_mon.status.volume_gain = 1.0f;
        s_mon.status.last_error = ESP_OK;
        s_mon.shared_stream_initialized = true;
    }
    portEXIT_CRITICAL(&s_mon.lock);

    ESP_LOGI(TAG, "shared AFE stream ready rate=%lu frame=%ums",
             (unsigned long)sample_rate_hz, (unsigned)frame_ms);
    return ESP_OK;
}

void audio_monitor_shared_stream_set_state(bool sleep_analysis_active, bool playback_active)
{
    portENTER_CRITICAL(&s_mon.lock);
    if (!s_mon.shared_stream_initialized) {
        portEXIT_CRITICAL(&s_mon.lock);
        return;
    }

    bool was_active = s_mon.shared_stream_active;
    bool was_playback = s_mon.playback_active;
    s_mon.shared_stream_active = sleep_analysis_active;
    s_mon.playback_active = playback_active;
    s_mon.status.running = sleep_analysis_active;
    if (!sleep_analysis_active || playback_active) {
        s_mon.status.snore_active = false;
        s_mon.status.quiet_seconds = 0;
        s_mon.quiet_started_us = 0;
    }
    if (!sleep_analysis_active || playback_active) {
        reset_snore_candidate_locked();
        reset_snore_sequence_locked();
        s_mon.snore_refractory_until_us = 0;
        s_mon.snore_analysis_started_us = 0;
    } else if (!was_active || was_playback) {
        /* Recalibrate after entering deep sleep or after loud local playback. */
        reset_snore_candidate_locked();
        s_mon.snore_refractory_until_us = 0;
        s_mon.snore_analysis_started_us = esp_timer_get_time();
        s_mon.status.level_dbfs = AUDIO_MONITOR_MIN_DBFS;
        s_mon.status.noise_floor_dbfs = AUDIO_MONITOR_MIN_DBFS;
    }
    portEXIT_CRITICAL(&s_mon.lock);

    if (was_active != sleep_analysis_active || was_playback != playback_active) {
        ESP_LOGI(TAG, "shared stream analysis=%d playback_gate=%d",
                 sleep_analysis_active ? 1 : 0, playback_active ? 1 : 0);
        printf("DG Snore: analysis=%d playback_gate=%d\r\n",
               sleep_analysis_active ? 1 : 0, playback_active ? 1 : 0);
    }
}

void audio_monitor_shared_stream_set_sleep_context(bool person_present,
                                                   bool breath_valid,
                                                   float breath_bpm)
{
    int64_t now_us = esp_timer_get_time();
    bool valid_breath = breath_valid && breath_bpm >= 4.0f && breath_bpm <= 35.0f;
    portENTER_CRITICAL(&s_mon.lock);
    bool was_ready = s_mon.person_present && s_mon.breath_valid;
    if (person_present) {
        s_mon.snore_presence_last_seen_us = now_us;
    }
    if (valid_breath) {
        s_mon.snore_breath_last_seen_us = now_us;
        s_mon.breath_bpm = breath_bpm;
    }
    s_mon.person_present = s_mon.snore_presence_last_seen_us > 0 &&
                           now_us - s_mon.snore_presence_last_seen_us <=
                               SNORE_PRESENCE_GRACE_US;
    s_mon.breath_valid = s_mon.snore_breath_last_seen_us > 0 &&
                         now_us - s_mon.snore_breath_last_seen_us <=
                             SNORE_BREATH_GRACE_US;
    if (!s_mon.breath_valid) {
        s_mon.breath_bpm = 0.0f;
    }
    s_mon.snore_context_update_us = now_us;
    bool ready = s_mon.person_present && s_mon.breath_valid;
    if (!ready) {
        reset_snore_candidate_locked();
        reset_snore_sequence_locked();
        s_mon.snore_analysis_started_us = 0;
    } else if (!was_ready && s_mon.shared_stream_active && !s_mon.playback_active) {
        reset_snore_candidate_locked();
        reset_snore_sequence_locked();
        s_mon.snore_analysis_started_us = now_us;
        s_mon.status.level_dbfs = AUDIO_MONITOR_MIN_DBFS;
        s_mon.status.noise_floor_dbfs = AUDIO_MONITOR_MIN_DBFS;
    }
    portEXIT_CRITICAL(&s_mon.lock);
    if (was_ready != ready) {
        printf("DG Snore: radar_gate=%d presence=%d breath_valid=%d breath=%.1f\r\n",
               ready ? 1 : 0, s_mon.person_present ? 1 : 0,
               s_mon.breath_valid ? 1 : 0, (double)s_mon.breath_bpm);
    }
}

void audio_monitor_process_shared_samples(const int16_t *samples, size_t sample_count,
                                          bool afe_speech)
{
    if (!samples || sample_count == 0) {
        return;
    }

    bool analyze = false;
    portENTER_CRITICAL(&s_mon.lock);
    int64_t now_us = esp_timer_get_time();
    bool context_fresh = s_mon.snore_context_update_us > 0 &&
                         now_us - s_mon.snore_context_update_us <= SNORE_CONTEXT_MAX_AGE_US;
    analyze = s_mon.shared_stream_initialized && s_mon.shared_stream_active &&
              !s_mon.playback_active && s_mon.person_present &&
              s_mon.breath_valid && context_fresh;
    if (!analyze) {
        reset_snore_candidate_locked();
        reset_snore_sequence_locked();
    }
    portEXIT_CRITICAL(&s_mon.lock);
    if (!analyze) {
        return;
    }

    int64_t sum_sq = 0;
    int32_t peak = 0;
    uint32_t zero_cross = 0;
    int16_t prev = samples[0];
    for (size_t i = 0; i < sample_count; ++i) {
        int32_t sample = samples[i];
        sum_sq += (int64_t)sample * (int64_t)sample;
        int32_t abs_sample = sample < 0 ? -sample : sample;
        if (abs_sample > peak) {
            peak = abs_sample;
        }
        if (i > 0 && ((prev < 0 && sample >= 0) || (prev >= 0 && sample < 0))) {
            zero_cross++;
        }
        prev = (int16_t)sample;
    }

    float rms = sqrtf((float)sum_sq / (float)sample_count);
    float zcr = (float)zero_cross / (float)sample_count;
    update_status(rms, (float)peak, zcr, now_us, false, false, afe_speech);

    audio_monitor_status_t snapshot = {0};
    bool log_status = false;
    portENTER_CRITICAL(&s_mon.lock);
    if (s_mon.last_shared_status_log_us == 0 ||
        now_us - s_mon.last_shared_status_log_us >= 10000000LL) {
        s_mon.last_shared_status_log_us = now_us;
        snapshot = s_mon.status;
        log_status = true;
    }
    portEXIT_CRITICAL(&s_mon.lock);
    if (log_status) {
        printf("DG Snore: frames=%u active=%d events=%u rejected=%u level=%.1f floor=%.1f\r\n",
               (unsigned)snapshot.frames, snapshot.snore_active ? 1 : 0,
               (unsigned)snapshot.snore_events,
               (unsigned)snapshot.snore_rejected_events, (double)snapshot.level_dbfs,
               (double)snapshot.noise_floor_dbfs);
    }
}

esp_err_t audio_monitor_start(const audio_monitor_config_t *config)
{
    if (s_mon.started) {
        return ESP_ERR_INVALID_STATE;
    }
    audio_monitor_config_t cfg;
    if (config) {
        cfg = *config;
    } else {
        audio_monitor_get_default_config(&cfg);
    }
    s_mon.cfg = cfg;
    memset(&s_mon.status, 0, sizeof(s_mon.status));
    s_mon.status.enabled = cfg.enabled;
    s_mon.status.sample_rate_hz = cfg.sample_rate_hz;
    s_mon.status.frame_ms = cfg.frame_ms;
    s_mon.status.level_dbfs = AUDIO_MONITOR_MIN_DBFS;
    s_mon.status.noise_floor_dbfs = AUDIO_MONITOR_MIN_DBFS;
    s_mon.status.volume_gain = 1.0f;
    s_mon.status.wake_word_configured = true;
    if (!cfg.enabled) {
        return ESP_OK;
    }

    s_mon.mic = bsp_audio_codec_microphone_init();
    if (!s_mon.mic) {
        s_mon.status.last_error = ESP_ERR_NOT_FOUND;
        ESP_LOGW(TAG, "microphone codec init failed");
        return ESP_ERR_NOT_FOUND;
    }

    BaseType_t ok = xTaskCreate(audio_monitor_task, "audio_monitor",
                                AUDIO_MONITOR_TASK_STACK, NULL,
                                AUDIO_MONITOR_TASK_PRIORITY, NULL);
    if (ok != pdPASS) {
        s_mon.status.last_error = ESP_ERR_NO_MEM;
        return ESP_ERR_NO_MEM;
    }
    s_mon.started = true;
    ESP_LOGI(TAG, "microphone monitor starting rate=%lu frame=%ums",
             (unsigned long)cfg.sample_rate_hz, (unsigned)cfg.frame_ms);
    return ESP_OK;
}

void audio_monitor_get_status(audio_monitor_status_t *status)
{
    if (!status) {
        return;
    }
    portENTER_CRITICAL(&s_mon.lock);
    *status = s_mon.status;
    portEXIT_CRITICAL(&s_mon.lock);
}

float audio_monitor_volume_gain(void)
{
    float gain = 1.0f;
    portENTER_CRITICAL(&s_mon.lock);
    if (s_mon.status.running) {
        gain = s_mon.status.volume_gain;
    }
    portEXIT_CRITICAL(&s_mon.lock);
    return gain;
}

bool audio_monitor_voice_activity(void)
{
    bool active;
    portENTER_CRITICAL(&s_mon.lock);
    active = s_mon.status.running && s_mon.status.voice_activity;
    portEXIT_CRITICAL(&s_mon.lock);
    return active;
}

bool audio_monitor_take_wake_request(void)
{
    bool pending = false;
    portENTER_CRITICAL(&s_mon.lock);
    pending = s_mon.wake_pending;
    s_mon.wake_pending = false;
    portEXIT_CRITICAL(&s_mon.lock);
    return pending;
}
