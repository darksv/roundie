#include "audio_diagnostics.h"
#include "sdkconfig.h"
#if CONFIG_ROUNDIE_DEV_AUDIO
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "roundie_audio.h"

void roundie_audio_command(char *line, bool discard)
{
    char id[9] = "00000000";
    esp_err_t error = ESP_ERR_INVALID_ARG;
    size_t length = strlen(line);
    if (length >= 14 && memcmp(line, "!RA1 ", 5) == 0 && line[13] == ' ') {
        bool valid_id = true;
        for (size_t index = 5; index < 13; index++) {
            valid_id = valid_id && ((line[index] >= '0' && line[index] <= '9') ||
                                    (line[index] >= 'a' && line[index] <= 'f'));
        }
        if (valid_id) {
            memcpy(id, line + 5, 8);
        }
        const char *operation = line + 14;
        if (!valid_id || discard || length >= 64) {
            error = ESP_ERR_INVALID_ARG;
        } else if (strcmp(operation, "STATUS") == 0) {
            error = ESP_OK;
        } else if (strcmp(operation, "STOP") == 0) {
            roundie_audio_stop();
            error = ESP_OK;
        } else if (strcmp(operation, "RELEASE") == 0) {
            /* Releases the self-test stream as an application unload would. */
            error = roundie_audio_release_owner(ROUNDIE_AUDIO_OWNER_DIAGNOSTIC, 500) ? ESP_OK : ESP_ERR_TIMEOUT;
        } else if (strncmp(operation, "STREAM ", 7) == 0) {
            /* STREAM <hz> <ms> <volume> <stall ms>: a producer writes a tone into a stream. */
            char *end = NULL;
            long frequency = strtol(operation + 7, &end, 10);
            long duration = end != NULL && *end == ' ' ? strtol(end + 1, &end, 10) : -1;
            long volume = end != NULL && *end == ' ' ? strtol(end + 1, &end, 10) : -1;
            long stall = end != NULL && *end == ' ' ? strtol(end + 1, &end, 10) : -1;
            if (end == NULL || *end != '\0' || frequency < 0 || duration < 0 || volume < 0 || stall < 0) {
                error = ESP_ERR_INVALID_ARG;
            } else {
                error = roundie_audio_stream_selftest((uint32_t)frequency, (uint32_t)duration,
                                                      (uint8_t)(volume > 255 ? 255 : volume), (uint32_t)stall);
            }
        } else if (strncmp(operation, "MIC ", 4) == 0) {
            /* MIC <ms>: record from the microphones; the result is a log line. */
            char *end = NULL;
            long duration = strtol(operation + 4, &end, 10);
            error = end != NULL && *end == '\0' && duration >= 0 ?
                roundie_audio_mic_test((uint32_t)duration) : ESP_ERR_INVALID_ARG;
        } else if (strncmp(operation, "MELODY ", 7) == 0) {
            char *end = NULL;
            long volume = strtol(operation + 7, &end, 10);
            error = end != NULL && *end == '\0' && volume >= 0 ?
                roundie_audio_play_melody((uint8_t)(volume > 255 ? 255 : volume)) : ESP_ERR_INVALID_ARG;
        } else if (strcmp(operation, "REGS") == 0) {
            /* Codec registers 0x00..0x45 as hex, for comparing with the datasheet. */
            char dump[2 * 0x46 + 1];
            for (unsigned reg = 0; reg < 0x46; reg++) {
                uint8_t value = 0xff;
                if (roundie_audio_read_register((uint8_t)reg, &value) != ESP_OK) {
                    memcpy(dump, "READFAIL", 8);
                    dump[8] = '\0';
                    break;
                }
                snprintf(dump + 2 * reg, 3, "%02x", value);
            }
            char reply[2 * 0x46 + 64];
            int out = snprintf(reply, sizeof(reply), "\n!RA1 %s 0 {\"regs\":\"%s\"}\n", id, dump);
            if (out > 0 && (size_t)out < sizeof(reply)) {
                usb_serial_jtag_write_bytes(reply, (size_t)out, pdMS_TO_TICKS(100));
            }
            return;
        } else if (strncmp(operation, "TONE ", 5) == 0) {
            char *end = NULL;
            long frequency = strtol(operation + 5, &end, 10);
            long duration = end != NULL && *end == ' ' ? strtol(end + 1, &end, 10) : -1;
            long volume = end != NULL && *end == ' ' ? strtol(end + 1, &end, 10) : -1;
            if (end == NULL || *end != '\0' || frequency < 0 || duration < 0 || volume < 0) {
                error = ESP_ERR_INVALID_ARG;
            } else {
                error = roundie_audio_play_tone((uint32_t)frequency, (uint32_t)duration, (uint8_t)(volume > 255 ? 255 : volume));
            }
        }
    }
    roundie_audio_status_t status;
    roundie_audio_status(&status);
    char response[420];
    int length_out = snprintf(response, sizeof(response),
        "\n!RA1 %s %d {\"ready\":%u,\"step\":\"%s\",\"error\":%d,\"ack\":%u,\"id\":\"%02x%02x%02x\","
        "\"playing\":%u,\"plays\":%u,\"errors\":%u,\"volume\":%u,\"cap\":%u,"
        "\"stream\":%u,\"phase\":%u,\"ring\":%u,\"underruns\":%u,\"pauses\":%u,\"master\":%u}\n",
        id, (int)error, status.initialized, status.init_step != NULL ? status.init_step : "none",
        (int)status.init_error, status.codec_ack, status.codec_id[0], status.codec_id[1], status.codec_id[2],
        status.playing || roundie_audio_busy(), (unsigned)status.plays, (unsigned)status.errors,
        status.last_volume, (unsigned)ROUNDIE_AUDIO_MAX_PERCENT, status.stream_open, status.stream_phase,
        (unsigned)status.ring_used, (unsigned)status.underruns, (unsigned)status.pauses,
        status.master_percent);
    if (length_out > 0 && (size_t)length_out < sizeof(response)) {
        usb_serial_jtag_write_bytes(response, (size_t)length_out, pdMS_TO_TICKS(100));
    }
}
#else
void roundie_audio_command(char *line, bool discard)
{
    (void)line;
    (void)discard;
}
#endif
