/*
 * Copyright 2017-2022 GoPro Inc.
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

#include <stdio.h>
#include <stdlib.h>

#include "common.h"

double clipf64(double v, double min, double max)
{
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

int clipi32(int v, int min, int max)
{
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

int64_t clipi64(int64_t v, int64_t min, int64_t max)
{
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

#define BUF_SIZE 1024

char *get_text_file_content(const char *filename)
{
    char *buf = NULL;

    FILE *fp = filename ? fopen(filename, "rb") : stdin;
    if (!fp) {
        fprintf(stderr, "unable to open %s\n", filename);
        goto end;
    }

    size_t pos = 0;
    for (;;) {
        const size_t needed = pos + BUF_SIZE + 1;
        void *new_buf = realloc(buf, needed);
        if (!new_buf) {
            free(buf);
            buf = NULL;
            goto end;
        }
        buf = new_buf;
        const size_t n = fread(buf + pos, 1, BUF_SIZE, fp);
        if (ferror(fp)) {
            free(buf);
            buf = NULL;
            goto end;
        }
        pos += n;
        if (feof(fp)) {
            buf[pos] = 0;
            break;
        }
    }

end:
    if (fp && fp != stdin)
        fclose(fp);
    return buf;
}
