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

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include "export/export.h"
#include "nopeaudio/nopeaudio.h"

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                             \
            return -1;                                                                             \
        }                                                                                          \
    } while (0)


/* Format the decoder reports for the encoder formats of the tests */
static enum AVSampleFormat av_sample_format(enum nau_sample_format format)
{
    switch (format) {
    case NAU_SAMPLE_FORMAT_S16:  return AV_SAMPLE_FMT_S16;
    case NAU_SAMPLE_FORMAT_S32:  return AV_SAMPLE_FMT_S32;
    case NAU_SAMPLE_FORMAT_F32P: return AV_SAMPLE_FMT_FLTP;
    default:                     return AV_SAMPLE_FMT_NONE;
    }
}
static float sample(int i, int channel) { return channel ? .25f : (float)(i % 5 - 2) * .125f; }

static int receive(AVCodecContext *decoder, AVFrame *frame, int *decoded, int expected,
                   enum AVSampleFormat input_format, double *energy)
{
    int ret;
    while ((ret = avcodec_receive_frame(decoder, frame)) >= 0) {
        CHECK(frame->ch_layout.nb_channels == 2 && frame->sample_rate == 48000);
        if (decoder->codec_id == AV_CODEC_ID_FLAC) {
            CHECK(*decoded + frame->nb_samples <= expected);
            CHECK(frame->format == input_format);
            for (int i = 0; i < frame->nb_samples; i++) {
                for (int c = 0; c < 2; c++) {
                    const float value = sample(*decoded + i, c);
                    if (input_format == AV_SAMPLE_FMT_S16)
                        CHECK(((int16_t *)frame->data[0])[2 * i + c] == lrintf(value * 32767.f));
                    else
                        CHECK(((int32_t *)frame->data[0])[2 * i + c] ==
                              (int32_t)(value * 2147483648.f));
                }
            }
        } else {
            CHECK(frame->format == AV_SAMPLE_FMT_FLTP);
            for (int c = 0; c < 2; c++) {
                for (int i = 0; i < frame->nb_samples; i++) {
                    const float value = ((float *)frame->data[c])[i];
                    CHECK(isfinite(value));
                    *energy += (double)value * value;
                }
            }
        }
        *decoded += frame->nb_samples;
        av_frame_unref(frame);
    }
    CHECK(ret == AVERROR(EAGAIN) || ret == AVERROR_EOF);
    return 0;
}

static int test_codec(const char *filename, const struct nau_exporter_config *config, int frames)
{
    const AVCodec *codec = avcodec_find_encoder_by_name(config->encoder);
    if (!codec || !av_guess_format(config->container, NULL, NULL)) {
        fprintf(stderr, "Skipping unavailable %s/%s\n", config->encoder, config->container);
        return 0;
    }
    struct nau_exporter *e = nau_exporter_create();
    CHECK(e);
    CHECK(nau_exporter_open(e, filename, config) == NAU_OK);
    const uint32_t chunks[] = {1, 17, 4096, 257};
    float samples[4096 * 2];
    for (int written = 0, chunk = 0; written < frames; chunk++) {
        const uint32_t n = FFMIN((uint32_t)(frames - written), chunks[chunk % 4]);
        for (uint32_t i = 0; i < n; i++) {
            samples[2 * i] = sample(written + (int)i, 0);
            samples[2 * i + 1] = sample(written + (int)i, 1);
        }
        CHECK(nau_exporter_write(e, samples, n) == NAU_OK);
        written += (int)n;
    }
    CHECK(nau_exporter_closep(&e) == NAU_OK && !e);

    AVFormatContext *input = NULL;
    CHECK(avformat_open_input(&input, filename, NULL, NULL) == 0);
    CHECK(avformat_find_stream_info(input, NULL) >= 0);
    CHECK(input->nb_streams == 1);
    AVCodecParameters *par = input->streams[0]->codecpar;
    CHECK(par->codec_id == codec->id && par->sample_rate == 48000 &&
          par->ch_layout.nb_channels == 2);
    AVCodecContext *decoder = avcodec_alloc_context3(avcodec_find_decoder(par->codec_id));
    CHECK(decoder && avcodec_parameters_to_context(decoder, par) == 0);
    CHECK(avcodec_open2(decoder, NULL, NULL) == 0);
    AVPacket *packet = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    CHECK(packet && frame);
    int decoded = 0, ret;
    double energy = 0;
    while ((ret = av_read_frame(input, packet)) >= 0) {
        CHECK(avcodec_send_packet(decoder, packet) == 0);
        CHECK(receive(decoder, frame, &decoded, frames, av_sample_format(config->sample_format), &energy) == 0);
        av_packet_unref(packet);
    }
    CHECK(ret == AVERROR_EOF);
    CHECK(avcodec_send_packet(decoder, NULL) == 0);
    CHECK(receive(decoder, frame, &decoded, frames, av_sample_format(config->sample_format), &energy) == 0);
    if (codec->id == AV_CODEC_ID_FLAC)
        CHECK(decoded == frames);
    else
        CHECK(decoded >= frames && decoded < frames + 2048 && energy > 0);
    av_frame_free(&frame);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&input);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 1;
    const struct nau_exporter_option flac_options[] = {{"compression_level", "5"}, {NULL, NULL}};
    const struct nau_exporter_option mp4_options[] = {{"movflags", "+faststart"}, {NULL, NULL}};
    const struct nau_exporter_config configs[] = {
        {.container = "flac",
         .encoder = "flac",
         .sample_format = NAU_SAMPLE_FORMAT_S16,
         .sample_rate = 48000,
         .encoder_options = flac_options},
        {.container = "matroska",
         .encoder = "flac",
         .sample_format = NAU_SAMPLE_FORMAT_S32,
         .sample_rate = 48000},
        {.container = "mp4",
         .encoder = "aac",
         .sample_format = NAU_SAMPLE_FORMAT_F32P,
         .sample_rate = 48000,
         .bit_rate = 128000,
         .container_options = mp4_options},
    };
    int ret = 0;
    for (size_t i = 0; !ret && i < sizeof(configs) / sizeof(*configs); i++) {
        ret = test_codec(argv[1], &configs[i], 17);
        if (!ret)
            ret = test_codec(argv[1], &configs[i], 10001);
    }
    remove(argv[1]);
    return ret != 0;
}
