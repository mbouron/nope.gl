/*
 * Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
 *
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <libavutil/intfloat.h>
#include <libavutil/intreadwrite.h>

#include "nopeaudio/nopeaudio.h"
#include "export/export.h"

#define CHECK(cond) do {                                                     \
    if (!(cond)) {                                                           \
        fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);             \
        return -1;                                                           \
    }                                                                        \
} while (0)

/* Check the file independently of FFmpeg's demuxer. Leave f at the samples. */
static int read_header(FILE *f, enum nau_export_format format, uint32_t rate, uint32_t frames)
{
    uint8_t b[40];
    CHECK(fread(b, 1, 12, f) == 12);
    CHECK(!memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4));
    const uint32_t riff_size = AV_RL32(b + 4);
    CHECK(fseek(f, 0, SEEK_END) == 0);
    CHECK(ftell(f) == (int64_t)riff_size + 8);
    CHECK(fseek(f, 12, SEEK_SET) == 0);
    int have_fmt = 0, have_fact = 0;
    const uint32_t bits = format == NAU_EXPORT_S16 ? 16 : 32;
    while (fread(b, 1, 8, f) == 8) {
        const uint32_t size = AV_RL32(b + 4);
        if (!memcmp(b, "data", 4)) {
            CHECK(have_fmt && (format == NAU_EXPORT_S16 || have_fact));
            CHECK(size == frames * 2 * bits / 8);
            return 0;
        }
        const long next = ftell(f) + (long)size + (size & 1);
        if (!memcmp(b, "fmt ", 4)) {
            CHECK(size >= 16 && fread(b, 1, 16, f) == 16);
            CHECK(AV_RL16(b) == (format == NAU_EXPORT_S16 ? 1 : 3));
            CHECK(AV_RL16(b + 2) == 2 && AV_RL32(b + 4) == rate);
            CHECK(AV_RL32(b + 8) == rate * 2 * bits / 8);
            CHECK(AV_RL16(b + 12) == 2 * bits / 8 && AV_RL16(b + 14) == bits);
            have_fmt = 1;
        } else if (!memcmp(b, "fact", 4)) {
            CHECK(size >= 4 && fread(b, 1, 4, f) == 4);
            CHECK(AV_RL32(b) == frames);
            have_fact = 1;
        }
        CHECK(fseek(f, next, SEEK_SET) == 0);
    }
    CHECK(0);
}

/* Same configuration as nau_player_export() */
static int open_wav(struct nau_exporter *e, const char *filename, enum nau_export_format format, uint32_t rate)
{
    const struct nau_exporter_option options[] = {{"rf64", "auto"}, {NULL, NULL}};
    const struct nau_exporter_config config = {
        .container = "wav",
        .encoder = format == NAU_EXPORT_S16 ? "pcm_s16le" : "pcm_f32le",
        .sample_format = format == NAU_EXPORT_S16 ? NAU_SAMPLE_FORMAT_S16 : NAU_SAMPLE_FORMAT_F32,
        .sample_rate = rate,
        .container_options = options,
        .empty_packet = 1,
    };
    return nau_exporter_open(e, filename, &config);
}

static int test_lifecycle(const char *filename, const char *directory)
{
    struct nau_exporter *e = nau_exporter_create();
    CHECK(e);
    CHECK(nau_exporter_write(e, NULL, 0) == NAU_ERROR_INVALID);
    CHECK(nau_exporter_closep(&e) == NAU_OK && !e);
    e = nau_exporter_create();
    CHECK(e);
    CHECK(open_wav(e, directory, NAU_EXPORT_S16, 48000) == NAU_ERROR_IO);
    CHECK(nau_exporter_closep(&e) == NAU_OK && !e);
    e = nau_exporter_create();
    CHECK(e);
    CHECK(open_wav(e, directory, NAU_EXPORT_S16, 48000) == NAU_ERROR_IO);
    CHECK(open_wav(e, filename, NAU_EXPORT_S16, 48000) == NAU_OK);
    CHECK(open_wav(e, filename, NAU_EXPORT_S16, 48000) == NAU_ERROR_INVALID);
    CHECK(nau_exporter_closep(&e) == NAU_OK && !e);
    return 0;
}

static int test_samples(const char *filename, enum nau_export_format format)
{
    const float samples[] = {0.f, .5f, -.5f, 1.f, -1.f, 2.f, -2.f, NAN, INFINITY, -INFINITY};
    const int16_t s16[] = {0, 16384, -16384, 32767, -32767, 32767, -32767, 0, 0, 0};
    struct nau_exporter *e = nau_exporter_create();
    CHECK(e);
    CHECK(open_wav(e, filename, format, 44100) == NAU_OK);
    CHECK(nau_exporter_write(e, NULL, 0) == NAU_OK);
    CHECK(nau_exporter_write(e, samples, UINT32_MAX) == NAU_ERROR_INVALID);
    CHECK(nau_exporter_write(e, samples, 2) == NAU_OK);
    CHECK(nau_exporter_write(e, samples + 4, 1) == NAU_OK);
    CHECK(nau_exporter_write(e, samples + 6, 2) == NAU_OK);
    CHECK(nau_exporter_closep(&e) == NAU_OK && !e);
    CHECK(nau_exporter_closep(&e) == NAU_OK);

    FILE *f = fopen(filename, "rb");
    CHECK(f);
    CHECK(read_header(f, format, 44100, 5) == 0);
    for (size_t i = 0; i < sizeof(samples) / sizeof(*samples); i++) {
        uint8_t b[4];
        const size_t size = format == NAU_EXPORT_S16 ? 2 : 4;
        CHECK(fread(b, 1, size, f) == size);
        if (format == NAU_EXPORT_S16) {
            CHECK(AV_RL16(b) == (uint16_t)s16[i]);
        } else {
            const float value = av_int2float(AV_RL32(b));
            CHECK(isnan(samples[i]) ? isnan(value) : value == samples[i]);
        }
    }
    CHECK(fgetc(f) == EOF && !ferror(f));
    CHECK(fclose(f) == 0);
    return 0;
}

static int test_player(const char *filename, const char *directory, enum nau_export_format format)
{
    const struct nau_player_config config = {.mode = NAU_PLAYER_OFFLINE};
    struct nau_player *player = nau_player_create();
    CHECK(player);
    CHECK(nau_player_init(player, &config) == NAU_OK);
    CHECK(nau_player_set_looping(player, 1) == NAU_OK);
    /* Empty project, then a partial final block and a repeated export. */
    const uint32_t durations[] = {0, 8193, 1};
    for (size_t i = 0; i < sizeof(durations) / sizeof(*durations); i++) {
        CHECK(nau_player_set_duration(player, durations[i]) == NAU_OK);
        CHECK(nau_player_export(player, filename, format) == NAU_OK);
        CHECK(!nau_player_is_playing(player) && nau_player_is_looping(player));
        FILE *f = fopen(filename, "rb");
        CHECK(f);
        CHECK(read_header(f, format, 48000, durations[i]) == 0);
        int c;
        while ((c = fgetc(f)) != EOF)
            CHECK(c == 0);
        CHECK(!ferror(f) && fclose(f) == 0);
    }
    CHECK(nau_player_export(player, directory, format) == NAU_ERROR_IO);
    CHECK(nau_player_export(player, filename, format) == NAU_OK);
    nau_player_freep(&player);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 3)
        return 1;
    int ret = test_lifecycle(argv[1], argv[2]);
    if (!ret)
        ret = test_samples(argv[1], NAU_EXPORT_S16);
    if (!ret)
        ret = test_samples(argv[1], NAU_EXPORT_F32);
    if (!ret)
        ret = test_player(argv[1], argv[2], NAU_EXPORT_S16);
    if (!ret)
        ret = test_player(argv[1], argv[2], NAU_EXPORT_F32);
    remove(argv[1]);
    return ret != 0;
}
