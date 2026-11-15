#include "roundie_audio.h"
#include <math.h>
#include <stdatomic.h>
#include <string.h>
#include "esp_codec_dev.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "roundie_board.h"
#include "roundie_stream.h"
#include "roundie_tone.h"

#define CHUNK_FRAMES 256U
#define TONE_AMPLITUDE 16000 /* half of full scale: headroom for the codec and the amplifier */
#define TASK_STACK 4096
#define TASK_PRIORITY 3
#define TONE_MAX_MS 5000U

static const char *TAG = "audio";

typedef struct {
    uint32_t frequency_hz;
    uint32_t duration_ms;
    uint8_t volume_percent;
    bool melody;
    bool wake; /* only wakes the task: a stream was opened */
} tone_request_t;

typedef struct {
    uint16_t frequency_hz;
    uint16_t duration_ms;
} note_t;

/* Pachelbel's Canon in D, the theme only, one voice. Eighth, quarter and half notes at 120 ms,
 * 240 ms and 480 ms; pitches in D major (F#5 740, E5 659, D5 587, C#5 554, B4 494, A4 440,
 * G4 392, F#4 370, E4 330). */
#define HALF 480
#define QUARTER 240
#define EIGHTH 120
static const note_t melody[] = {
    {740, HALF}, {659, HALF}, {587, HALF}, {554, HALF}, {494, HALF}, {440, HALF}, {494, HALF}, {554, HALF},
    {587, HALF}, {554, HALF}, {494, HALF}, {440, HALF}, {392, HALF}, {370, HALF}, {392, HALF}, {330, HALF},
    {740, QUARTER}, {659, QUARTER}, {587, QUARTER}, {554, QUARTER}, {494, QUARTER}, {440, QUARTER},
    {494, QUARTER}, {554, QUARTER}, {587, QUARTER}, {554, QUARTER}, {494, QUARTER}, {440, QUARTER},
    {392, QUARTER}, {370, QUARTER}, {392, QUARTER}, {330, QUARTER},
    {740, EIGHTH}, {659, EIGHTH}, {587, EIGHTH}, {554, EIGHTH}, {494, EIGHTH}, {440, EIGHTH}, {494, EIGHTH},
    {554, EIGHTH}, {587, EIGHTH}, {554, EIGHTH}, {494, EIGHTH}, {440, EIGHTH}, {392, EIGHTH}, {370, EIGHTH},
    {392, EIGHTH}, {330, EIGHTH}, {587, 960},
};
#define NOTE_GAP_FRAMES 320U /* 20 ms of silence between notes */

static esp_codec_dev_handle_t device;
static esp_codec_dev_handle_t microphone; /* NULL without a board microphone */
static roundie_board_t *audio_board; /* for the register read; set once by init */
static QueueHandle_t requests;
static roundie_audio_status_t status;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static _Atomic bool busy; /* a tone, the melody or a stream owns the codec */
static _Atomic bool stop_requested;
static _Atomic uint8_t master_percent = ROUNDIE_AUDIO_MASTER_DEFAULT;

/* The one PCM stream. Fields below the mutex line are written under stream_mutex; the ring is lock-free
 * (the owner writes, the audio task reads) and its storage is freed only by the audio task. */
typedef struct {
    bool open;
    bool closing;
    bool drain;
    uint32_t owner;
    uint32_t generation;
    uint32_t sample_rate;
    uint8_t channels;
    uint32_t gain_q15;
    uint8_t *storage;
    roundie_ring_t ring;
    roundie_playback_t playback;
    bool codec_open;
} stream_t;

static stream_t stream;
static uint32_t generation_counter;
static SemaphoreHandle_t stream_mutex;
static uint32_t total_underruns;
static uint32_t total_pauses;
#define MUTEX_WAIT_MS 50
/* One tick (10 ms at CONFIG_FREERTOS_HZ=100). pdMS_TO_TICKS() of a shorter time is 0,
 * which turns a wait into a busy loop that starves lower-priority tasks such as the GUI. */
#define POLL_TICKS ((TickType_t)1)

static esp_err_t fail(const char *step, esp_err_t error)
{
    portENTER_CRITICAL(&status_lock);
    status.init_error = error;
    status.init_step = step;
    portEXIT_CRITICAL(&status_lock);
    ESP_LOGE(TAG, "%s: %s", step, esp_err_to_name(error));
    return error;
}

static void count_error(void)
{
    portENTER_CRITICAL(&status_lock);
    status.errors++;
    portEXIT_CRITICAL(&status_lock);
}

/* One note through the open codec. False after a write failure. */
static bool play_note(uint32_t frequency_hz, uint32_t duration_ms, int16_t *chunk)
{
    roundie_tone_t tone;
    if (roundie_tone_init(&tone, frequency_hz, ROUNDIE_AUDIO_SAMPLE_RATE, duration_ms, TONE_AMPLITUDE) == 0) {
        return true;
    }
    while (!atomic_load(&stop_requested)) {
        uint32_t frames = roundie_tone_fill(&tone, chunk, CHUNK_FRAMES);
        if (frames == 0) {
            break;
        }
        if (esp_codec_dev_write(device, chunk, (int)(frames * 2U * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
            count_error();
            return false;
        }
    }
    return true;
}

static void play(const tone_request_t *request)
{
    uint8_t volume = (uint8_t)((uint32_t)request->volume_percent * ROUNDIE_AUDIO_MAX_PERCENT / 100U);
    portENTER_CRITICAL(&status_lock);
    status.playing = true;
    status.last_volume = volume;
    portEXIT_CRITICAL(&status_lock);

    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = 16,
        .channel = 2,
        .sample_rate = ROUNDIE_AUDIO_SAMPLE_RATE,
    };
    /* Opening the codec also switches the amplifier on and enables I2S. */
    if (esp_codec_dev_open(device, &format) != ESP_CODEC_DEV_OK) {
        count_error();
    } else {
        if (esp_codec_dev_set_out_vol(device, (float)volume) != ESP_CODEC_DEV_OK) {
            count_error();
        }
        static int16_t chunk[2U * CHUNK_FRAMES];
        if (request->melody) {
            for (size_t index = 0; index < sizeof(melody) / sizeof(melody[0]) && !atomic_load(&stop_requested); index++) {
                if (!play_note(melody[index].frequency_hz, melody[index].duration_ms, chunk)) {
                    break;
                }
                memset(chunk, 0, sizeof(chunk));
                esp_codec_dev_write(device, chunk, (int)(NOTE_GAP_FRAMES * 2U * sizeof(int16_t)));
            }
        } else {
            play_note(request->frequency_hz, request->duration_ms, chunk);
        }
        /* A little silence keeps the end from being cut by the close. */
        memset(chunk, 0, sizeof(chunk));
        esp_codec_dev_write(device, chunk, (int)sizeof(chunk));
        esp_codec_dev_close(device);
    }
    portENTER_CRITICAL(&status_lock);
    status.playing = false;
    status.plays++;
    portEXIT_CRITICAL(&status_lock);
}

static bool valid_rate(uint32_t rate)
{
    return rate == 8000U || rate == 16000U || rate == 24000U || rate == 32000U || rate == 48000U;
}

static uint8_t codec_volume_for_master(void)
{
    return (uint8_t)((uint32_t)atomic_load(&master_percent) * ROUNDIE_AUDIO_MAX_PERCENT / 100U);
}

/* Audio task only. Gives the codec and the ring back and clears the stream. */
static void stream_release(void)
{
    if (stream.codec_open) {
        static int16_t silence[2U * CHUNK_FRAMES];
        memset(silence, 0, sizeof(silence));
        esp_codec_dev_write(device, silence, (int)sizeof(silence));
        esp_codec_dev_close(device);
        stream.codec_open = false;
    }
    xSemaphoreTake(stream_mutex, portMAX_DELAY);
    total_underruns += stream.playback.underruns;
    total_pauses += stream.playback.pauses;
    heap_caps_free(stream.storage);
    stream.storage = NULL;
    stream.open = false;
    stream.closing = false;
    stream.drain = false;
    stream.owner = 0;
    portENTER_CRITICAL(&status_lock);
    status.playing = false;
    status.plays++;
    portEXIT_CRITICAL(&status_lock);
    atomic_store(&busy, false);
    xSemaphoreGive(stream_mutex);
}

/* Audio task only: decides and plays chunks until it has to wait. */
static void stream_service(void)
{
    static int16_t stereo[2U * CHUNK_FRAMES];
    static int16_t pcm[2U * CHUNK_FRAMES];
    const size_t chunk_bytes = (size_t)CHUNK_FRAMES * stream.channels * sizeof(int16_t);
    for (;;) {
        if (stream.closing && !stream.drain) {
            stream_release();
            return;
        }
        stream.playback.drain = stream.closing && stream.drain;
        roundie_playback_action_t action = roundie_playback_step(&stream.playback, roundie_ring_used(&stream.ring),
                                                                 chunk_bytes, esp_timer_get_time());
        if (action == ROUNDIE_PLAYBACK_ACTION_START) {
            esp_codec_dev_sample_info_t format = {
                .bits_per_sample = 16,
                .channel = 2,
                .sample_rate = stream.sample_rate,
            };
            uint8_t volume = codec_volume_for_master();
            if (esp_codec_dev_open(device, &format) != ESP_CODEC_DEV_OK) {
                count_error();
                stream_release();
                return;
            }
            stream.codec_open = true;
            portENTER_CRITICAL(&status_lock);
            status.playing = true;
            status.last_volume = volume;
            portEXIT_CRITICAL(&status_lock);
            if (esp_codec_dev_set_out_vol(device, (float)volume) != ESP_CODEC_DEV_OK) {
                count_error();
            }
            action = ROUNDIE_PLAYBACK_ACTION_PLAY;
        }
        if (action == ROUNDIE_PLAYBACK_ACTION_PLAY) {
            size_t got = roundie_ring_read(&stream.ring, pcm, chunk_bytes);
            uint32_t frames = (uint32_t)(got / ((size_t)stream.channels * sizeof(int16_t)));
            roundie_pcm_to_stereo(pcm, frames, stream.channels, stream.gain_q15, stereo);
            if (frames > 0 && esp_codec_dev_write(device, stereo, (int)(frames * 2U * sizeof(int16_t))) != ESP_CODEC_DEV_OK) {
                count_error();
                stream_release();
                return;
            }
        } else if (action == ROUNDIE_PLAYBACK_ACTION_SILENCE) {
            memset(stereo, 0, sizeof(stereo));
            if (esp_codec_dev_write(device, stereo, (int)sizeof(stereo)) != ESP_CODEC_DEV_OK) {
                count_error();
                stream_release();
                return;
            }
        } else if (action == ROUNDIE_PLAYBACK_ACTION_PAUSE) {
            esp_codec_dev_close(device);
            stream.codec_open = false;
            portENTER_CRITICAL(&status_lock);
            status.playing = false;
            portEXIT_CRITICAL(&status_lock);
        } else if (action == ROUNDIE_PLAYBACK_ACTION_FINISH) {
            stream_release();
            return;
        } else {
            return; /* WAIT: look at the queue again after one tick */
        }
    }
}

static void audio_task(void *argument)
{
    (void)argument;
    tone_request_t request;
    for (;;) {
        TickType_t wait = stream.open ? POLL_TICKS : portMAX_DELAY;
        if (xQueueReceive(requests, &request, wait) == pdTRUE && !request.wake) {
            play(&request);
            atomic_store(&stop_requested, false);
            atomic_store(&busy, false);
        }
        if (stream.open) {
            stream_service();
        }
    }
}

esp_err_t roundie_audio_init(roundie_board_t *board)
{
    status.init_step = "start";
    roundie_board_speaker_t speaker;
    esp_err_t error = roundie_board_speaker_start(board, ROUNDIE_AUDIO_SAMPLE_RATE, &speaker);
    status.codec_ack = speaker.codec_ack;
    memcpy(status.codec_id, speaker.codec_id, sizeof(status.codec_id));
    if (error != ESP_OK) {
        return fail(speaker.failed_step, error);
    }
    audio_board = board;
    device = speaker.device;
    microphone = speaker.microphone;
    stream_mutex = xSemaphoreCreateMutex();
    requests = xQueueCreate(1, sizeof(tone_request_t));
    if (stream_mutex == NULL || requests == NULL || xTaskCreate(audio_task, "audio", TASK_STACK, NULL, TASK_PRIORITY, NULL) != pdPASS) {
        return fail("audio task", ESP_ERR_NO_MEM);
    }
    portENTER_CRITICAL(&status_lock);
    status.initialized = true;
    status.init_error = ESP_OK;
    status.init_step = "ok";
    portEXIT_CRITICAL(&status_lock);
    ESP_LOGI(TAG, "Speaker ready, cap %u%%", ROUNDIE_AUDIO_MAX_PERCENT);
    return ESP_OK;
}

esp_err_t roundie_audio_play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent)
{
    if (!status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frequency_hz < ROUNDIE_TONE_MIN_HZ || frequency_hz > ROUNDIE_TONE_MAX_HZ || duration_ms < 10U ||
        duration_ms > TONE_MAX_MS || volume_percent > 100U) {
        return ESP_ERR_INVALID_ARG;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&busy, &expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&stop_requested, false);
    const tone_request_t request = {frequency_hz, duration_ms, volume_percent, false, false};
    if (xQueueSend(requests, &request, 0) != pdTRUE) {
        atomic_store(&busy, false);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

esp_err_t roundie_audio_play_melody(uint8_t volume_percent)
{
    if (!status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (volume_percent > 100U) {
        return ESP_ERR_INVALID_ARG;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&busy, &expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    atomic_store(&stop_requested, false);
    const tone_request_t request = {0, 0, volume_percent, true, false};
    if (xQueueSend(requests, &request, 0) != pdTRUE) {
        atomic_store(&busy, false);
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_OK;
}

void roundie_audio_stop(void)
{
    atomic_store(&stop_requested, true);
}

esp_err_t roundie_audio_read_register(uint8_t reg, uint8_t *value)
{
    if (audio_board == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return roundie_board_speaker_read_register(audio_board, reg, value);
}

bool roundie_audio_busy(void)
{
    return atomic_load(&busy);
}

void roundie_audio_status(roundie_audio_status_t *out)
{
    portENTER_CRITICAL(&status_lock);
    *out = status;
    portEXIT_CRITICAL(&status_lock);
    out->stream_open = stream.open;
    out->stream_phase = (uint8_t)stream.playback.phase;
    out->ring_used = stream.open ? (uint32_t)roundie_ring_used(&stream.ring) : 0;
    out->underruns = total_underruns + (stream.open ? stream.playback.underruns : 0);
    out->pauses = total_pauses + (stream.open ? stream.playback.pauses : 0);
    out->master_percent = atomic_load(&master_percent);
}

void roundie_audio_set_master(uint8_t percent)
{
    atomic_store(&master_percent, percent > 100U ? 100U : percent);
}

roundie_audio_stream_t roundie_audio_stream_open(uint32_t owner, uint32_t sample_rate, uint8_t channels,
                                                 uint8_t volume_percent)
{
    if (!status.initialized || !valid_rate(sample_rate) || (channels != 1U && channels != 2U) ||
        volume_percent > 100U) {
        return 0;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&busy, &expected, true)) {
        return 0;
    }
    uint8_t *storage = heap_caps_malloc(ROUNDIE_AUDIO_RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (storage == NULL || xSemaphoreTake(stream_mutex, pdMS_TO_TICKS(MUTEX_WAIT_MS)) != pdTRUE) {
        heap_caps_free(storage);
        atomic_store(&busy, false);
        return 0;
    }
    stream.storage = storage;
    roundie_ring_init(&stream.ring, storage, ROUNDIE_AUDIO_RING_BYTES);
    stream.owner = owner;
    stream.sample_rate = sample_rate;
    stream.channels = channels;
    stream.gain_q15 = (uint32_t)volume_percent * 32768U / 100U;
    stream.closing = false;
    stream.drain = false;
    stream.codec_open = false;
    size_t prebuffer = (size_t)sample_rate * channels * sizeof(int16_t) * ROUNDIE_AUDIO_PREBUFFER_MS / 1000U;
    if (prebuffer > ROUNDIE_AUDIO_RING_BYTES / 2U) {
        prebuffer = ROUNDIE_AUDIO_RING_BYTES / 2U; /* 48 kHz stereo: 100 ms would not fit, playback never starts */
    }
    roundie_playback_init(&stream.playback, prebuffer, (int64_t)ROUNDIE_AUDIO_PAUSE_AFTER_MS * 1000);
    stream.generation = ++generation_counter;
    if (stream.generation == 0) {
        stream.generation = ++generation_counter;
    }
    stream.open = true;
    uint32_t handle = stream.generation;
    xSemaphoreGive(stream_mutex);
    /* The task may be blocked with no timeout: wake it. The queue is empty because busy was free. */
    const tone_request_t wake = {.wake = true};
    xQueueSend(requests, &wake, 0);
    return handle;
}

int32_t roundie_audio_stream_write(roundie_audio_stream_t handle, const void *pcm, uint32_t bytes)
{
    if (handle == 0 || pcm == NULL || stream_mutex == NULL || xSemaphoreTake(stream_mutex, pdMS_TO_TICKS(MUTEX_WAIT_MS)) != pdTRUE) {
        return -1;
    }
    int32_t accepted = -1;
    if (stream.open && !stream.closing && stream.generation == handle) {
        /* Whole samples only, so a mono or stereo frame never splits. */
        uint32_t frame_bytes = (uint32_t)stream.channels * (uint32_t)sizeof(int16_t);
        uint32_t whole = bytes - bytes % frame_bytes;
        accepted = (int32_t)roundie_ring_write(&stream.ring, pcm, whole);
    }
    xSemaphoreGive(stream_mutex);
    return accepted;
}

int32_t roundie_audio_stream_writable(roundie_audio_stream_t handle)
{
    if (handle == 0 || stream_mutex == NULL || xSemaphoreTake(stream_mutex, pdMS_TO_TICKS(MUTEX_WAIT_MS)) != pdTRUE) {
        return -1;
    }
    int32_t free_bytes = -1;
    if (stream.open && !stream.closing && stream.generation == handle) {
        uint32_t frame_bytes = (uint32_t)stream.channels * (uint32_t)sizeof(int16_t);
        uint32_t space = (uint32_t)roundie_ring_free(&stream.ring);
        free_bytes = (int32_t)(space - space % frame_bytes);
    }
    xSemaphoreGive(stream_mutex);
    return free_bytes;
}

void roundie_audio_stream_close(roundie_audio_stream_t handle, bool drain)
{
    if (handle == 0 || stream_mutex == NULL || xSemaphoreTake(stream_mutex, pdMS_TO_TICKS(MUTEX_WAIT_MS)) != pdTRUE) {
        return;
    }
    if (stream.open && stream.generation == handle) {
        stream.drain = drain;
        stream.closing = true;
    }
    xSemaphoreGive(stream_mutex);
}

bool roundie_audio_release_owner(uint32_t owner, uint32_t timeout_ms)
{
    if (stream_mutex == NULL) {
        return true; /* audio never initialised: nobody owns a stream */
    }
    if (xSemaphoreTake(stream_mutex, pdMS_TO_TICKS(MUTEX_WAIT_MS)) != pdTRUE) {
        return false;
    }
    bool mine = stream.open && stream.owner == owner;
    if (mine) {
        stream.drain = false;
        stream.closing = true;
    }
    xSemaphoreGive(stream_mutex);
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (mine) {
        if (xSemaphoreTake(stream_mutex, pdMS_TO_TICKS(MUTEX_WAIT_MS)) == pdTRUE) {
            bool still = stream.open && stream.owner == owner;
            xSemaphoreGive(stream_mutex);
            if (!still) {
                return true;
            }
        }
        if (esp_timer_get_time() > deadline) {
            return false;
        }
        vTaskDelay(POLL_TICKS);
    }
    return true;
}

#if CONFIG_ROUNDIE_DEV_AUDIO
typedef struct {
    uint32_t frequency_hz;
    uint32_t duration_ms;
    uint8_t volume_percent;
    uint32_t stall_ms;
} selftest_t;

static _Atomic bool selftest_running;

static void selftest_task(void *argument)
{
    selftest_t test = *(selftest_t *)argument;
    heap_caps_free(argument);
    roundie_audio_stream_t handle = roundie_audio_stream_open(ROUNDIE_AUDIO_OWNER_DIAGNOSTIC, ROUNDIE_AUDIO_SAMPLE_RATE, 2, test.volume_percent);
    if (handle != 0) {
        roundie_tone_t tone;
        static int16_t buffer[2U * CHUNK_FRAMES];
        roundie_tone_init(&tone, test.frequency_hz, ROUNDIE_AUDIO_SAMPLE_RATE, test.duration_ms, TONE_AMPLITUDE);
        uint32_t half = tone.total_frames / 2U;
        bool stalled = test.stall_ms == 0;
        while (tone.frames_left > 0) {
            if (!stalled && tone.frames_done >= half) {
                stalled = true;
                vTaskDelay(pdMS_TO_TICKS(test.stall_ms)); /* the producer goes quiet: an underrun */
            }
            uint32_t frames = roundie_tone_fill(&tone, buffer, CHUNK_FRAMES);
            const uint8_t *cursor = (const uint8_t *)buffer;
            uint32_t left = frames * 4U;
            while (left > 0) {
                int32_t put = roundie_audio_stream_write(handle, cursor, left);
                if (put < 0) {
                    left = 0;
                    break;
                }
                cursor += put;
                left -= (uint32_t)put;
                if (left > 0) {
                    vTaskDelay(POLL_TICKS); /* the ring is full: wait for the consumer */
                }
            }
        }
        roundie_audio_stream_close(handle, true);
    }
    atomic_store(&selftest_running, false);
    vTaskDelete(NULL);
}

esp_err_t roundie_audio_stream_selftest(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent,
                                        uint32_t stall_ms)
{
    if (!status.initialized || atomic_load(&busy)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frequency_hz < ROUNDIE_TONE_MIN_HZ || frequency_hz > ROUNDIE_TONE_MAX_HZ || duration_ms < 100U ||
        duration_ms > TONE_MAX_MS || volume_percent > 100U || stall_ms > 3000U) {
        return ESP_ERR_INVALID_ARG;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&selftest_running, &expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    selftest_t *test = heap_caps_malloc(sizeof(*test), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (test == NULL) {
        atomic_store(&selftest_running, false);
        return ESP_ERR_NO_MEM;
    }
    *test = (selftest_t){frequency_hz, duration_ms, volume_percent, stall_ms};
    if (xTaskCreate(selftest_task, "audio_feed", 3072, test, 2, NULL) != pdPASS) {
        heap_caps_free(test);
        atomic_store(&selftest_running, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#define MIC_CHUNK_FRAMES 256U
#define MIC_CHANNELS 2U
#define MIC_READ_ERROR_LIMIT 10U
#define MIC_TEST_MAX_MS 5000U
#define MIC_GAIN_DB 30.0F /* from vendor examples; clips with nearby sound on the 1.75C */

/* Owns the codec (busy) for the whole recording, then releases it. */
static void mic_test_task(void *argument)
{
    uint32_t duration_ms = (uint32_t)(uintptr_t)argument;
    esp_codec_dev_sample_info_t format = {
        .bits_per_sample = 16,
        .channel = MIC_CHANNELS,
        .sample_rate = ROUNDIE_AUDIO_SAMPLE_RATE,
    };
    int32_t peak[MIC_CHANNELS] = {0, 0};
    uint64_t square_sum[MIC_CHANNELS] = {0, 0};
    uint32_t frames = 0;
    uint32_t read_errors = 0;
    int opened = esp_codec_dev_open(microphone, &format);
    if (opened == ESP_CODEC_DEV_OK) {
        esp_codec_dev_set_in_gain(microphone, MIC_GAIN_DB);
        static int16_t buffer[MIC_CHANNELS * MIC_CHUNK_FRAMES];
        uint32_t wanted = ROUNDIE_AUDIO_SAMPLE_RATE / 1000U * duration_ms;
        while (frames < wanted && read_errors < MIC_READ_ERROR_LIMIT) {
            if (esp_codec_dev_read(microphone, buffer, (int)sizeof(buffer)) != ESP_CODEC_DEV_OK) {
                read_errors++;
                continue;
            }
            for (uint32_t frame = 0; frame < MIC_CHUNK_FRAMES; frame++) {
                for (uint32_t channel = 0; channel < MIC_CHANNELS; channel++) {
                    int32_t sample = buffer[MIC_CHANNELS * frame + channel];
                    int32_t magnitude = sample < 0 ? -sample : sample;
                    if (magnitude > peak[channel]) {
                        peak[channel] = magnitude;
                    }
                    square_sum[channel] += (uint64_t)((int64_t)sample * sample);
                }
            }
            frames += MIC_CHUNK_FRAMES;
        }
        esp_codec_dev_close(microphone);
    }
    uint32_t rms[MIC_CHANNELS] = {0, 0};
    for (uint32_t channel = 0; channel < MIC_CHANNELS; channel++) {
        if (frames > 0) {
            rms[channel] = (uint32_t)sqrt((double)square_sum[channel] / (double)frames);
        }
    }
    ESP_LOGI(TAG, "Mic test %u ms: open=%d, frames=%u, read errors=%u, ch0 peak %d rms %u, "
             "ch1 peak %d rms %u (16-bit full scale 32767, gain %d dB)",
             (unsigned)duration_ms, opened, (unsigned)frames, (unsigned)read_errors,
             (int)peak[0], (unsigned)rms[0], (int)peak[1], (unsigned)rms[1], (int)MIC_GAIN_DB);
    atomic_store(&busy, false);
    vTaskDelete(NULL);
}

esp_err_t roundie_audio_mic_test(uint32_t duration_ms)
{
    if (!status.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (microphone == NULL) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (duration_ms < 100U || duration_ms > MIC_TEST_MAX_MS) {
        return ESP_ERR_INVALID_ARG;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&busy, &expected, true)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(mic_test_task, "mic_test", 3072, (void *)(uintptr_t)duration_ms, 2, NULL) != pdPASS) {
        atomic_store(&busy, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
#endif
