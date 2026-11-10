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

#ifndef NAU_EXPORT_H
#define NAU_EXPORT_H

#include <stdint.h>

struct nau_exporter;

/*
 * Native-endian encoder samples: U/S denote unsigned/signed integers and F
 * denotes floating point, followed by the bit width. Formats are interleaved
 * unless suffixed with P, which means one plane per channel.
 */
enum nau_sample_format {
    NAU_SAMPLE_FORMAT_U8,
    NAU_SAMPLE_FORMAT_S16,
    NAU_SAMPLE_FORMAT_S32,
    NAU_SAMPLE_FORMAT_S64,
    NAU_SAMPLE_FORMAT_F32,
    NAU_SAMPLE_FORMAT_F64,
    NAU_SAMPLE_FORMAT_U8P,
    NAU_SAMPLE_FORMAT_S16P,
    NAU_SAMPLE_FORMAT_S32P,
    NAU_SAMPLE_FORMAT_S64P,
    NAU_SAMPLE_FORMAT_F32P,
    NAU_SAMPLE_FORMAT_F64P,
    NAU_SAMPLE_FORMAT_MAX_ENUM = 0x7FFFFFFF
};

struct nau_exporter_option {
    const char *name;
    const char *value;
};

struct nau_exporter_config {
    const char *container;
    const char *encoder;
    enum nau_sample_format sample_format; /* encoder format; write() always takes interleaved float */
    uint32_t sample_rate;
    int64_t bit_rate;
    /* Optional arrays terminated by an entry with a NULL name. */
    const struct nau_exporter_option *encoder_options;
    const struct nau_exporter_option *container_options;
    /* Supply a timestamp on empty exports, e.g. for WAV's fact chunk. */
    int empty_packet;
};

struct nau_exporter *nau_exporter_create(void);

/* Open a local file. A failed open leaves the object available for another open. */
int nau_exporter_open(struct nau_exporter *exporter, const char *filename,
                      const struct nau_exporter_config *config);

/* Interleaved stereo float at config.sample_rate; chunks need not match encoder frames. */
int nau_exporter_write(struct nau_exporter *exporter, const float *samples, uint32_t frames);

/*
 * Drain the encoder, finalize the container, free the object and clear *exporterp.
 * Safe before open and after a failed open. Returns any write or finalization error.
 */
int nau_exporter_closep(struct nau_exporter **exporterp);

#endif
