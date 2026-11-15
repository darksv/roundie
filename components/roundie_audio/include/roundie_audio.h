#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

/* Speaker output (docs/audio.md): the board's codec device (roundie_board_speaker_start;
 * on the 1.85B the ES8311 and the NS4150B amplifier). A system audio task owns it;
 * nothing else touches it. */

typedef struct roundie_board roundie_board_t; /* roundie_board.h */

#define ROUNDIE_AUDIO_MAX_PERCENT 50U /* system cap on the codec output volume, an assumption */
#define ROUNDIE_AUDIO_SAMPLE_RATE 16000U       /* tones and the melody */
#define ROUNDIE_AUDIO_RING_BYTES 16384U         /* the stream's PCM ring, a power of two */
#define ROUNDIE_AUDIO_PREBUFFER_MS 100U         /* buffered before playback starts or resumes */
#define ROUNDIE_AUDIO_PAUSE_AFTER_MS 500U       /* an underrun this long closes the codec and the amplifier */
#define ROUNDIE_AUDIO_MASTER_DEFAULT 70U        /* system volume, a share of the cap */
#define ROUNDIE_AUDIO_OWNER_DIAGNOSTIC UINT32_MAX /* RA1 self-test; application instances never reach it */

/* Stream handles are generation counters: 0 is never valid, a stale one is refused. */
typedef uint32_t roundie_audio_stream_t;

typedef struct {
    bool initialized;
    esp_err_t init_error;      /* first failure, ESP_OK when initialised */
    const char *init_step;     /* the step that failed, or "ok" */
    bool codec_ack;            /* the codec answered on its control bus */
    uint8_t codec_id[3];       /* board-defined identity (1.85B: ES8311 registers 0xFD..0xFF) */
    bool playing;
    uint32_t plays;            /* tones that ran to their end or were stopped */
    uint32_t errors;           /* codec open, volume or write failures */
    uint8_t last_volume;       /* codec volume of the last tone or stream after the cap */
    bool stream_open;
    uint8_t stream_phase;      /* roundie_playback_phase_t */
    uint32_t ring_used;        /* bytes buffered */
    uint32_t underruns;        /* playing to underrun transitions, all streams */
    uint32_t pauses;           /* underruns that closed the codec */
    uint8_t master_percent;
} roundie_audio_status_t;

/* Once, after roundie_board_start(), on a board with ROUNDIE_BOARD_CAP_SPEAKER; not in
 * Safe Mode. Failure leaves audio unavailable and does not affect anything else. */
esp_err_t roundie_audio_init(roundie_board_t *board);
/* A tone from the system. volume_percent 0..100 is a share of the system cap.
 * Refused while a tone plays or when audio is unavailable. */
esp_err_t roundie_audio_play_tone(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent);
/* Development: a built-in single-voice melody (the theme of Pachelbel's Canon, simplified,
 * about 19 s) so the output can be judged by ear. Same volume rule as a tone. */
esp_err_t roundie_audio_play_melody(uint8_t volume_percent);
/* A PCM stream: 16-bit samples, mono or stereo, at 8, 16, 24, 32 or 48 kHz. One stream at a
 * time, never together with a tone. The owner is an opaque number (an application
 * instance); volume_percent is a software gain on the samples. The prebuffer is 100 ms,
 * at most half the ring (85 ms of ring in all at 48 kHz stereo). write copies into a ring
 * and never blocks: it returns the bytes accepted (0 when full) or -1 for a stale or
 * closed handle. Playback starts once the prebuffer is full; a drained ring plays
 * silence, then pauses. Returns 0 when refused. */
roundie_audio_stream_t roundie_audio_stream_open(uint32_t owner, uint32_t sample_rate, uint8_t channels,
                                                 uint8_t volume_percent);
int32_t roundie_audio_stream_write(roundie_audio_stream_t stream, const void *pcm, uint32_t bytes);
/* Bytes a write would accept now (whole frames), or -1 for a stale or closing handle. */
int32_t roundie_audio_stream_writable(roundie_audio_stream_t stream);
/* Asynchronous. drain plays what is buffered first; otherwise it is dropped at once. */
void roundie_audio_stream_close(roundie_audio_stream_t stream, bool drain);
/* Discards the owner's stream and waits (bounded) until the task released it. True when the
 * owner has no stream afterwards. For an application unload. */
bool roundie_audio_release_owner(uint32_t owner, uint32_t timeout_ms);
/* System volume as a share of the cap, applied when a stream or tone opens the codec. */
void roundie_audio_set_master(uint8_t percent);
#if CONFIG_ROUNDIE_DEV_AUDIO
/* Development: a producer task writes a tone into a stream, stalls for stall_ms in the
 * middle to provoke an underrun, then closes with drain. */
esp_err_t roundie_audio_stream_selftest(uint32_t frequency_hz, uint32_t duration_ms, uint8_t volume_percent,
                                        uint32_t stall_ms);
/* Development, boards with ROUNDIE_BOARD_CAP_MICROPHONE: records duration_ms (100..5000)
 * in a worker that owns the codec meanwhile, then logs the peak and RMS of each channel.
 * ESP_ERR_NOT_SUPPORTED without a microphone. */
esp_err_t roundie_audio_mic_test(uint32_t duration_ms);
#endif
/* Ends the tone in progress at its next chunk. */
void roundie_audio_stop(void);
bool roundie_audio_busy(void);
/* Development: one codec register over I2C, independent of whether a stream is open. */
esp_err_t roundie_audio_read_register(uint8_t reg, uint8_t *value);
void roundie_audio_status(roundie_audio_status_t *out);
