/*
 * Copyright 2024 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2016-2022 GoPro Inc.
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

#include "config.h"

#if HAVE_USELOCALE
# define _POSIX_C_SOURCE 200809L
# include <locale.h>
# ifdef __APPLE__
#  include <xlocale.h>
# endif
#elif defined(_WIN32)
# include <locale.h>
#endif

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "drawutils.h"
#include "hud.h"
#include "internal.h"
#include "log.h"
#include "math_utils.h"
#include <ngpu/ngpu.h>
#include "nopegl/nopegl.h"
#include "pipeline_compat.h"
#include "utils/memory.h"
#include "utils/time.h"

struct transforms_block {
    struct ngli_mat4 modelview_matrix;
    struct ngli_mat4 projection_matrix;
};

enum {
    LATENCY_UPDATE_CPU,
    LATENCY_DRAW_CPU,
    LATENCY_TOTAL_CPU,
    LATENCY_DRAW_GPU,
    NB_LATENCY
};

enum counter_id {
    COUNTER_NODES,
    COUNTER_RENDER_PASSES,
    COUNTER_DRAW_CALLS,
    COUNTER_COMPUTE_DISPATCHES,
    COUNTER_BUFFERS,
    COUNTER_BUFFER_MEMORY,
    COUNTER_TEXTURES,
    COUNTER_TEXTURE_MEMORY,
    NB_COUNTERS,
};

enum widget_type {
    WIDGET_LATENCY,
    WIDGET_COUNTER,
};

struct data_graph {
    int64_t *values;
    size_t nb_values;
    size_t count;
    size_t pos;
    int64_t min;
    int64_t max;
    int64_t amin; // all-time min
    int64_t amax; // all-time max
};

struct latency_measure {
    int64_t *times;
    int count;
    int pos;
    int64_t total_times;
};

struct widget_latency {
    struct latency_measure measures[NB_LATENCY];
};

struct widget_counter {
    uint64_t value;
    uint64_t total; /* only meaningful for COUNTER_FMT_RATIO */
};

struct widget {
    enum widget_type type;
    struct rect rect;
    int text_x, text_y;
    struct rect graph_rect;
    struct data_graph *data_graph;
    const void *user_data;
    void *priv_data;
};

struct widget_spec {
    int text_cols, text_rows;
    int graph_w, graph_h;
    size_t nb_data_graph;
    size_t priv_size;
    int (*init)(struct hud *s, struct widget *widget);
    void (*make_stats)(struct hud *s, struct widget *widget);
    void (*draw)(struct hud *s, struct widget *widget);
    void (*csv_header)(struct hud *s, struct widget *widget, struct bstr *dst);
    void (*csv_report)(struct hud *s, struct widget *widget, struct bstr *dst);
    void (*uninit)(struct hud *s, struct widget *widget);
};

NGLI_DECLARE_DARRAY_WITH_NAME(widget_darray, struct widget);

struct hud {
    struct ngl_ctx *ctx;
    struct ngli_frame_stats frame_stats;

    int measure_window;
    int refresh_rate[2];
    const char *export_filename;
    int scale;

#if HAVE_USELOCALE
    locale_t c_locale;
#endif
    struct widget_darray widgets;
    uint32_t bg_color_u32;
    FILE *fp_export;
    struct bstr *csv_line;
    struct canvas canvas;
    double refresh_rate_interval;
    double last_refresh_time;

    struct ngpu_pgcraft *crafter;
    struct ngpu_texture *texture;
    struct ngpu_buffer *coords;
    int32_t transforms_block_index;
    struct pipeline_compat *pipeline_compat;
    struct ngpu_graphics_state graphics_state;
};

#define WIDGET_PADDING 4
#define WIDGET_MARGIN  2

#define LATENCY_WIDGET_TEXT_LEN 20
/*
 * Widest counter label ("Texture memory") is 14 characters, and the values are
 * either small counts, a "n/n" ratio or a byte count with a unit suffix.
 */
#define COUNTER_WIDGET_TEXT_LEN 15
#define COUNTERS_PER_ROW        4

static const struct {
    const char *label;
    const uint32_t color;
    char unit;
} latency_specs[] = {
    [LATENCY_UPDATE_CPU] = {"update CPU", 0xF43DF4FF, 'u'},
    [LATENCY_DRAW_CPU]   = {"draw   CPU", 0x3DF4F4FF, 'u'},
    [LATENCY_TOTAL_CPU]  = {"total  CPU", 0xF4F43DFF, 'u'},
    [LATENCY_DRAW_GPU]   = {"draw   GPU", 0x3DF43DFF, 'n'},
};

enum counter_fmt {
    COUNTER_FMT_COUNT,  /* plain number */
    COUNTER_FMT_RATIO,  /* value out of a total */
    COUNTER_FMT_MEMORY, /* number of bytes, with a unit suffix */
};

static const struct counter_spec {
    enum counter_id id;
    const char *label;
    uint32_t color;
    enum counter_fmt fmt;
} counter_specs[] = {
    #define COUNTER(id, label, color, fmt) [id] = {id, label, color, fmt}
    COUNTER(COUNTER_NODES,              "Nodes",          0xD632FFFF, COUNTER_FMT_RATIO),
    COUNTER(COUNTER_RENDER_PASSES,      "Render passes",  0x3284FFFF, COUNTER_FMT_COUNT),
    COUNTER(COUNTER_DRAW_CALLS,         "Draw calls",     0x32FF84FF, COUNTER_FMT_COUNT),
    COUNTER(COUNTER_COMPUTE_DISPATCHES, "Dispatches",     0xD6FF32FF, COUNTER_FMT_COUNT),
    COUNTER(COUNTER_BUFFERS,            "Buffers",        0xF43DF4FF, COUNTER_FMT_COUNT),
    COUNTER(COUNTER_BUFFER_MEMORY,      "Buffer memory",  0x3DF4F4FF, COUNTER_FMT_MEMORY),
    COUNTER(COUNTER_TEXTURES,           "Textures",       0xF4F43DFF, COUNTER_FMT_COUNT),
    COUNTER(COUNTER_TEXTURE_MEMORY,     "Texture memory", 0x3DF43DFF, COUNTER_FMT_MEMORY),
    #undef COUNTER
};

NGLI_STATIC_ASSERT(NGLI_ARRAY_NB(latency_specs) == NB_LATENCY, "hud nb latency");
NGLI_STATIC_ASSERT(NGLI_ARRAY_NB(counter_specs) == NB_COUNTERS, "hud nb counters");

/* Widget init */

static int widget_latency_init(struct hud *s, struct widget *widget)
{
    struct widget_latency *priv = widget->priv_data;

    ngli_assert(NB_LATENCY == NGLI_ARRAY_NB(priv->measures));

    s->measure_window = NGLI_MAX(s->measure_window, 1);
    for (size_t i = 0; i < NB_LATENCY; i++) {
        int64_t *times = ngli_try_calloc((size_t)s->measure_window, sizeof(*times));
        if (!times)
            return NGL_ERROR_MEMORY;
        priv->measures[i].times = times;
    }

    return 0;
}

/* Widget update */

static void register_time(struct hud *s, struct latency_measure *m, int64_t t)
{
    m->total_times = m->total_times - m->times[m->pos] + t;
    m->times[m->pos] = t;
    m->pos = (m->pos + 1) % s->measure_window;
    m->count = NGLI_MIN(m->count + 1, s->measure_window);
}

/* Widget make stats */

static void widget_latency_make_stats(struct hud *s, struct widget *widget)
{
    const struct ngli_frame_stats *stats = &s->frame_stats;
    struct widget_latency *priv = widget->priv_data;

    register_time(s, &priv->measures[LATENCY_UPDATE_CPU], stats->cpu_update_time);
    register_time(s, &priv->measures[LATENCY_DRAW_CPU],   stats->cpu_draw_time);
    register_time(s, &priv->measures[LATENCY_TOTAL_CPU],  stats->cpu_update_time + stats->cpu_draw_time);
    register_time(s, &priv->measures[LATENCY_DRAW_GPU],   stats->gpu_draw_time);
}

static void get_counter(const struct hud *s, enum counter_id id, struct widget_counter *dst)
{
    const struct ngli_frame_stats *stats = &s->frame_stats;

    *dst = (struct widget_counter){0};
    switch (id) {
    case COUNTER_NODES:
        dst->value = stats->active_node_count;
        dst->total = stats->node_count;
        return;
    case COUNTER_RENDER_PASSES:
        dst->value = stats->gpu.render_passes;
        return;
    case COUNTER_DRAW_CALLS:
        dst->value = stats->gpu.draw_calls;
        return;
    case COUNTER_COMPUTE_DISPATCHES:
        dst->value = stats->gpu.compute_dispatches;
        return;
    case COUNTER_BUFFERS:
        dst->value = stats->memory.buffer_count;
        return;
    case COUNTER_BUFFER_MEMORY:
        dst->value = stats->memory.buffer_bytes;
        return;
    case COUNTER_TEXTURES:
        dst->value = stats->memory.texture_count;
        return;
    case COUNTER_TEXTURE_MEMORY:
        dst->value = stats->memory.texture_bytes;
        return;
    case NB_COUNTERS:
        break;
    }
    ngli_assert(0);
}

static void widget_counter_make_stats(struct hud *s, struct widget *widget)
{
    const struct counter_spec *spec = widget->user_data;
    get_counter(s, spec->id, widget->priv_data);
}

/* Draw utils */

static inline uint8_t *set_color(uint8_t *p, uint32_t rgba)
{
    p[0] = (uint8_t)(rgba >> 24);
    p[1] = rgba >> 16 & 0xff;
    p[2] = rgba >>  8 & 0xff;
    p[3] = rgba       & 0xff;
    return p + 4;
}

static inline int get_pixel_pos(struct hud *s, int px, int py)
{
    return (py * s->canvas.w + px) * 4;
}

static inline void set_color_at(struct hud *s, int px, int py, uint32_t rgba)
{
    uint8_t *p = s->canvas.buf + get_pixel_pos(s, px, py);
    set_color(p, rgba);
}

static inline void set_color_at_column(struct hud *s, int px, int py, int height, uint32_t rgba)
{
    uint8_t *p = s->canvas.buf + get_pixel_pos(s, px, py);
    const int sign = height >= 0 ? 1 : -1;
    for (int h = 0; h < height; h++) {
        set_color(p, rgba);
        p += sign * s->canvas.w * 4;
    }
}

static void draw_block_graph(struct hud *s,
                             const struct data_graph *d,
                             const struct rect *rect,
                             int64_t graph_min, int64_t graph_max,
                             const uint32_t c)
{
    const int64_t graph_h = graph_max - graph_min;
    const float vscale = graph_h ? (float)rect->h / (float)graph_h : 0.f;
    const size_t start = (d->nb_values + d->pos - d->count) % d->nb_values;

    for (size_t k = 0; k < d->count; k++) {
        const int64_t v = d->values[(start + k) % d->nb_values];
        const int h = (int)((float)(v - graph_min) * vscale);
        const int y = NGLI_CLAMP(rect->h - h, 0, rect->h);
        set_color_at_column(s, rect->x + (int)k, rect->y + y, h, c);
    }
}

static void draw_line_graph(struct hud *s,
                            const struct data_graph *d,
                            const struct rect *rect,
                            int64_t graph_min, int64_t graph_max,
                            const uint32_t c)
{
    const int64_t graph_h = graph_max - graph_min;
    const float vscale = graph_h ? (float)rect->h / (float)graph_h : 0.f;
    const size_t start = (d->nb_values + d->pos - d->count) % d->nb_values;
    int prev_y;

    for (size_t k = 0; k < d->count; k++) {
        const int64_t v = d->values[(start + k) % d->nb_values];
        const int h = (int)((float)(v - graph_min) * vscale);
        const int y = NGLI_CLAMP(rect->h - 1 - h, 0, rect->h - 1);

        set_color_at(s, rect->x + (int)k, rect->y + y, c);
        if (k)
            set_color_at_column(s, rect->x + (int)k, rect->y + prev_y, y - prev_y, c);
        prev_y = y;
    }
}

static void print_text(struct hud *s, int x, int y, const char *buf, const uint32_t c)
{
    ngli_drawutils_print(&s->canvas, x, y, buf, c);
}

static void widgets_clear(struct hud *s)
{
    struct widget_darray *widgets_array = &s->widgets;
    struct widget *widgets = widgets_array->data;
    for (size_t i = 0; i < widgets_array->count; i++)
        ngli_drawutils_draw_rect(&s->canvas, &widgets[i].rect, s->bg_color_u32);
}

/* Widget draw */

static void register_graph_value(struct data_graph *d, int64_t v)
{
    const int64_t old_v = d->values[d->pos];

    d->values[d->pos] = v;
    d->pos = (d->pos + 1) % d->nb_values;
    d->count = NGLI_MIN(d->count + 1, d->nb_values);

    /* update min */
    if (old_v == d->min) {
        d->min = d->values[0];
        for (int i = 1; i < d->nb_values; i++)
            d->min = NGLI_MIN(d->min, d->values[i]);
    } else if (v < d->min) {
        d->min = v;
    }
    d->amin = NGLI_MIN(d->amin, d->min);

    /* update max */
    if (old_v == d->max) {
        d->max = d->values[0];
        for (int i = 1; i < d->nb_values; i++)
            d->max = NGLI_MAX(d->max, d->values[i]);
    } else if (v > d->max) {
        d->max = v;
    }
    d->amax = NGLI_MAX(d->amax, d->max);
}

static int64_t get_latency_avg(const struct widget_latency *priv, size_t id)
{
    const struct latency_measure *m = &priv->measures[id];
    return m->total_times / m->count / (latency_specs[id].unit == 'u' ? 1 : 1000);
}

static void widget_latency_draw(struct hud *s, struct widget *widget)
{
    struct widget_latency *priv = widget->priv_data;

    char buf[LATENCY_WIDGET_TEXT_LEN + 1];
    for (size_t i = 0; i < NB_LATENCY; i++) {
        const int64_t t = get_latency_avg(priv, i);

        snprintf(buf, sizeof(buf), "%s %5" PRId64 "usec", latency_specs[i].label, t);
        print_text(s, widget->text_x, widget->text_y + (int)i * NGLI_FONT_H, buf, latency_specs[i].color);
        register_graph_value(&widget->data_graph[i], t);
    }

    int64_t graph_min = widget->data_graph[0].min;
    int64_t graph_max = widget->data_graph[0].max;
    for (size_t i = 1; i < NB_LATENCY; i++) {
        graph_min = NGLI_MIN(graph_min, widget->data_graph[i].min);
        graph_max = NGLI_MAX(graph_max, widget->data_graph[i].max);
    }

    const int64_t graph_h = graph_max - graph_min;
    if (graph_h) {
        for (size_t i = 0; i < NB_LATENCY; i++)
            draw_line_graph(s, &widget->data_graph[i], &widget->graph_rect,
                            graph_min, graph_max, latency_specs[i].color);
    }
}

static void widget_counter_draw(struct hud *s, struct widget *widget)
{
    const struct widget_counter *priv = widget->priv_data;
    const struct counter_spec *spec = widget->user_data;

    char buf[COUNTER_WIDGET_TEXT_LEN + 1];
    if (spec->fmt == COUNTER_FMT_RATIO)
        snprintf(buf, sizeof(buf), "%"PRIu64"/%"PRIu64, priv->value, priv->total);
    else if (spec->fmt == COUNTER_FMT_MEMORY && priv->value >= 1024 * 1024 * 1024)
        snprintf(buf, sizeof(buf), "%"PRIu64"G", priv->value / (1024 * 1024 * 1024));
    else if (spec->fmt == COUNTER_FMT_MEMORY && priv->value >= 1024 * 1024)
        snprintf(buf, sizeof(buf), "%"PRIu64"M", priv->value / (1024 * 1024));
    else if (spec->fmt == COUNTER_FMT_MEMORY && priv->value >= 1024)
        snprintf(buf, sizeof(buf), "%"PRIu64"K", priv->value / 1024);
    else
        snprintf(buf, sizeof(buf), "%"PRIu64, priv->value);
    print_text(s, widget->text_x, widget->text_y, spec->label, spec->color);
    print_text(s, widget->text_x, widget->text_y + NGLI_FONT_H, buf, spec->color);

    struct data_graph *d = &widget->data_graph[0];
    const int64_t value = (int64_t)NGLI_MIN(priv->value, INT64_MAX);
    register_graph_value(d, value);
    if (d->amin != d->amax)
        draw_block_graph(s, d, &widget->graph_rect, d->amin, d->amax, spec->color);
}

/* Widget CSV header */

static void widget_latency_csv_header(struct hud *s, struct widget *widget, struct bstr *dst)
{
    for (size_t i = 0; i < NB_LATENCY; i++)
        ngli_bstr_printf(dst, "%s%s", i ? "," : "", latency_specs[i].label);
}

static void widget_counter_csv_header(struct hud *s, struct widget *widget, struct bstr *dst)
{
    const struct counter_spec *spec = widget->user_data;
    if (spec->fmt == COUNTER_FMT_RATIO)
        ngli_bstr_printf(dst, "%s active,%s total", spec->label, spec->label);
    else
        ngli_bstr_print(dst, spec->label);
}

/* Widget CSV report */

static void widget_latency_csv_report(struct hud *s, struct widget *widget, struct bstr *dst)
{
    const struct widget_latency *priv = widget->priv_data;
    for (size_t i = 0; i < NB_LATENCY; i++) {
        const int64_t t = get_latency_avg(priv, i);
        ngli_bstr_printf(dst, "%s%"PRId64, i ? "," : "", t);
    }
}

static void widget_counter_csv_report(struct hud *s, struct widget *widget, struct bstr *dst)
{
    const struct counter_spec *spec = widget->user_data;
    const struct widget_counter *priv = widget->priv_data;
    if (spec->fmt == COUNTER_FMT_RATIO)
        ngli_bstr_printf(dst, "%"PRIu64",%"PRIu64, priv->value, priv->total);
    else
        ngli_bstr_printf(dst, "%"PRIu64, priv->value);
}

/* Widget uninit */

static void widget_latency_uninit(struct hud *s, struct widget *widget)
{
    struct widget_latency *priv = widget->priv_data;
    for (size_t i = 0; i < NB_LATENCY; i++)
        ngli_free(priv->measures[i].times);
}

static const struct widget_spec widget_specs[] = {
    [WIDGET_LATENCY] = {
        .text_cols     = LATENCY_WIDGET_TEXT_LEN,
        .text_rows     = NB_LATENCY,
        .graph_w       = 320,
        .nb_data_graph = NB_LATENCY,
        .priv_size     = sizeof(struct widget_latency),
        .init          = widget_latency_init,
        .make_stats    = widget_latency_make_stats,
        .draw          = widget_latency_draw,
        .csv_header    = widget_latency_csv_header,
        .csv_report    = widget_latency_csv_report,
        .uninit        = widget_latency_uninit,
    },
    [WIDGET_COUNTER]  = {
        .text_cols     = COUNTER_WIDGET_TEXT_LEN,
        .text_rows     = 2,
        .graph_h       = 40,
        .nb_data_graph = 1,
        .priv_size     = sizeof(struct widget_counter),
        .make_stats    = widget_counter_make_stats,
        .draw          = widget_counter_draw,
        .csv_header    = widget_counter_csv_header,
        .csv_report    = widget_counter_csv_report,
    },
};

static inline int get_widget_width(enum widget_type type)
{
    const struct widget_spec *spec = &widget_specs[type];
    const int horizontal_layout = !spec->graph_h;
    return spec->graph_w
         + spec->text_cols * NGLI_FONT_W
         + WIDGET_PADDING * (2 + horizontal_layout);
}

static inline int get_widget_height(enum widget_type type)
{
    const struct widget_spec *spec = &widget_specs[type];
    const int vertical_layout = !!spec->graph_h;
    return spec->graph_h
         + spec->text_rows * NGLI_FONT_H
         + WIDGET_PADDING * (2 + vertical_layout);
}

static int create_widget(struct hud *s, enum widget_type type, const void *user_data, int x, int y)
{
    if (x < 0)
        x = s->canvas.w + x;
    if (y < 0)
        y = s->canvas.h + y;

    const struct widget_spec *spec = &widget_specs[type];

    ngli_assert(spec->text_cols && spec->text_rows);
    ngli_assert(spec->graph_w ^ spec->graph_h);
    ngli_assert(spec->nb_data_graph);

    const int horizontal_layout = !spec->graph_h;
    struct widget widget = {
        .type      = type,
        .rect.x    = x,
        .rect.y    = y,
        .rect.w    = get_widget_width(type),
        .rect.h    = get_widget_height(type),
        .text_x    = x + WIDGET_PADDING,
        .text_y    = y + WIDGET_PADDING,
        .user_data = user_data,
    };

    if (horizontal_layout) {
        widget.graph_rect.x = x + spec->text_cols * NGLI_FONT_W + WIDGET_PADDING * 2;
        widget.graph_rect.y = y + WIDGET_PADDING;
        widget.graph_rect.w = spec->graph_w;
        widget.graph_rect.h = widget.rect.h - WIDGET_PADDING * 2;
    } else {
        widget.graph_rect.x = x + WIDGET_PADDING;
        widget.graph_rect.y = y + spec->text_rows * NGLI_FONT_H + WIDGET_PADDING * 2;
        widget.graph_rect.w = widget.rect.w - WIDGET_PADDING * 2;
        widget.graph_rect.h = spec->graph_h;
    }

    if (ngli_darray_try_push(&s->widgets, widget) < 0)
        return NGL_ERROR_MEMORY;
    struct widget *widgetp = ngli_darray_tail(&s->widgets);

    widgetp->priv_data = ngli_try_calloc(1, spec->priv_size);
    if (!widgetp->priv_data)
        return NGL_ERROR_MEMORY;

    widgetp->data_graph = ngli_try_calloc(spec->nb_data_graph, sizeof(*widgetp->data_graph));
    if (!widgetp->data_graph)
        return NGL_ERROR_MEMORY;
    for (size_t i = 0; i < spec->nb_data_graph; i++) {
        struct data_graph *d = &widgetp->data_graph[i];
        d->nb_values = (size_t)widgetp->graph_rect.w;
        d->values = ngli_try_calloc(d->nb_values, sizeof(*d->values));
        if (!d->values)
            return NGL_ERROR_MEMORY;
    }

    return 0;
}

static void reset_widget(void *user_arg, void *data)
{
    struct hud *s = user_arg;
    struct widget *widget = data;
    if (widget_specs[widget->type].uninit)
        widget_specs[widget->type].uninit(s, widget);
    ngli_free(widget->priv_data);
    for (size_t i = 0; i < widget_specs[widget->type].nb_data_graph; i++)
        ngli_free(widget->data_graph[i].values);
    ngli_free(widget->data_graph);
}

static int widgets_init(struct hud *s)
{
    ngli_darray_set_free_func(&s->widgets, reset_widget, s);

    /* Smallest dimensions possible (in pixels) */
    const int latency_width = get_widget_width(WIDGET_LATENCY);
    const int counter_rows = (NB_COUNTERS + COUNTERS_PER_ROW - 1) / COUNTERS_PER_ROW;
    const int counters_width = get_widget_width(WIDGET_COUNTER) * COUNTERS_PER_ROW
                             + WIDGET_MARGIN * (COUNTERS_PER_ROW - 1);

    s->canvas.w = WIDGET_MARGIN * 2
                + NGLI_MAX(latency_width, counters_width);

    s->canvas.h = WIDGET_MARGIN * 3
                + get_widget_height(WIDGET_LATENCY)
                + get_widget_height(WIDGET_COUNTER) * counter_rows
                + WIDGET_MARGIN * (counter_rows - 1);

    /* Latency widget in the top-left */
    const int x_latency = WIDGET_MARGIN;
    const int y_latency = WIDGET_MARGIN;
    int ret = create_widget(s, WIDGET_LATENCY, NULL, x_latency, y_latency);
    if (ret < 0)
        return ret;

    const int y_counters = WIDGET_MARGIN + y_latency + get_widget_height(WIDGET_LATENCY);
    const int x_counter_step = get_widget_width(WIDGET_COUNTER) + WIDGET_MARGIN;
    const int y_counter_step = get_widget_height(WIDGET_COUNTER) + WIDGET_MARGIN;
    for (size_t i = 0; i < NB_COUNTERS; i++) {
        const int x_counter = WIDGET_MARGIN + (int)(i % COUNTERS_PER_ROW) * x_counter_step;
        const int y_counter = y_counters + (int)(i / COUNTERS_PER_ROW) * y_counter_step;
        ret = create_widget(s, WIDGET_COUNTER, &counter_specs[i], x_counter, y_counter);
        if (ret < 0)
            return ret;
    }

    /* Call init on every widget */
    struct widget_darray *widgets_array = &s->widgets;
    struct widget *widgets = widgets_array->data;
    for (size_t i = 0; i < widgets_array->count; i++) {
        struct widget *widget = &widgets[i];
        if (widget_specs[widget->type].init) {
            ret = widget_specs[widget->type].init(s, widget);
            if (ret < 0)
                return ret;
        }
    }

    return 0;
}

static void widgets_make_stats(struct hud *s)
{
    struct widget_darray *widgets_array = &s->widgets;
    struct widget *widgets = widgets_array->data;
    for (size_t i = 0; i < widgets_array->count; i++) {
        struct widget *widget = &widgets[i];
        widget_specs[widget->type].make_stats(s, widget);
    }
}

static void widgets_draw(struct hud *s)
{
    struct widget_darray *widgets_array = &s->widgets;
    struct widget *widgets = widgets_array->data;
    for (size_t i = 0; i < widgets_array->count; i++) {
        struct widget *widget = &widgets[i];
        widget_specs[widget->type].draw(s, widget);
    }
}

static int widgets_csv_header(struct hud *s)
{
    s->fp_export = fopen(s->export_filename, "wb");
    if (!s->fp_export) {
        LOG(ERROR, "unable to open \"%s\" for writing", s->export_filename);
        return NGL_ERROR_IO;
    }

    s->csv_line = ngli_bstr_create();
    if (!s->csv_line)
        return NGL_ERROR_MEMORY;

    ngli_bstr_print(s->csv_line, "time,");

    struct widget_darray *widgets_array = &s->widgets;
    struct widget *widgets = widgets_array->data;
    for (size_t i = 0; i < widgets_array->count; i++) {
        struct widget *widget = &widgets[i];
        ngli_bstr_print(s->csv_line, i ? "," : "");
        widget_specs[widget->type].csv_header(s, widget, s->csv_line);
    }

    ngli_bstr_print(s->csv_line, "\n");

    const size_t len = ngli_bstr_len(s->csv_line);
    size_t n = fwrite(ngli_bstr_strptr(s->csv_line), 1, len, s->fp_export);
    if (n != len) {
        LOG(ERROR, "unable to write CSV header");
        return NGL_ERROR_IO;
    }

    return 0;
}

static void widgets_csv_report(struct hud *s)
{
    const struct ngl_ctx *ctx = s->ctx;
    const struct ngl_node *scene = ctx->scene ? ctx->scene->params.root : NULL;

    /*
     * Set C locale temporarily so floats are printed deterministically. We
     * don't know how the API user is handling the locale, so we do it at the
     * beginning of the inner draw function and restore it at its end.
     */
#if HAVE_USELOCALE
    const locale_t prev_locale = uselocale(s->c_locale);
#elif defined(_WIN32)
    const char *prev_locale = setlocale(LC_ALL, NULL);
    if (!setlocale(LC_ALL, "C"))
        LOG(ERROR, "unable to set C locale");
#endif

    ngli_bstr_clear(s->csv_line);
    ngli_bstr_printf(s->csv_line, "%f", scene ? scene->last_update_time : 0);

    struct widget_darray *widgets_array = &s->widgets;
    struct widget *widgets = widgets_array->data;
    for (size_t i = 0; i < widgets_array->count; i++) {
        ngli_bstr_print(s->csv_line, ",");
        struct widget *widget = &widgets[i];
        widget_specs[widget->type].csv_report(s, widget, s->csv_line);
    }
    ngli_bstr_print(s->csv_line, "\n");

    const size_t len = ngli_bstr_len(s->csv_line);
    fwrite(ngli_bstr_strptr(s->csv_line), 1, len, s->fp_export);

#if HAVE_USELOCALE
    uselocale(prev_locale);
#elif defined(_WIN32)
    setlocale(LC_ALL, prev_locale);
#endif
}

static void widgets_uninit(struct hud *s)
{
    ngli_darray_reset(&s->widgets);
}

static const char * const vertex_data =
    "void main()"                                                           "\n"
    "{"                                                                     "\n"
    "    ngl_out_pos = projection_matrix"                                   "\n"
    "                * modelview_matrix"                                    "\n"
    "                * vec4(coords.xy, 0.0, 1.0);"                          "\n"
    "    tex_coord = coords.zw;"                                            "\n"
    "}";

static const char * const fragment_data =
    "void main()"                                                           "\n"
    "{"                                                                     "\n"
    "    ngl_out_color = texture(tex, tex_coord);"                          "\n"
    "}";

static const struct ngpu_pgcraft_iovar vert_out_vars[] = {
    {.name = "tex_coord", .type = NGPU_TYPE_VEC2},
};

struct hud *ngli_hud_create(struct ngl_ctx *ctx)
{
    struct hud *s = ngli_try_calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->ctx = ctx;
    return s;
}

int ngli_hud_init(struct hud *s)
{
    struct ngl_ctx *ctx = s->ctx;
    const struct ngl_config *config = &ctx->config;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;

    s->scale = config->hud_scale;
    s->measure_window = config->hud_measure_window;
    s->refresh_rate[0] = config->hud_refresh_rate[0];
    s->refresh_rate[1] = config->hud_refresh_rate[1];
    s->export_filename = config->hud_export_filename;
    s->scale = config->hud_scale;

    if (!s->measure_window)
        s->measure_window = 60;

    if (s->refresh_rate[1])
        s->refresh_rate_interval = s->refresh_rate[0] / (double)s->refresh_rate[1];
    s->last_refresh_time = -1;

    int ret = widgets_init(s);
    if (ret < 0)
        return ret;

    if (s->export_filename) {
#if HAVE_USELOCALE
        s->c_locale = newlocale(LC_CTYPE_MASK, "C", (locale_t)0);
        if (!s->c_locale) {
            LOG(ERROR, "unable to create C locale");
            return NGL_ERROR_EXTERNAL;
        }
#elif defined(_WIN32)
        _configthreadlocale(_ENABLE_PER_THREAD_LOCALE);
#else
        LOG(WARNING, "no locale support found, assuming C is currently in use");
#endif

        return widgets_csv_header(s);
    }

    s->canvas.buf = ngli_try_calloc((size_t)s->canvas.w * (size_t)s->canvas.h, 4);
    if (!s->canvas.buf)
        return NGL_ERROR_MEMORY;

    static const float bg_color[] = {0.0f, 0.0f, 0.0f, 0.8f};
    s->bg_color_u32 = NGLI_COLOR_VEC4_TO_U32(bg_color);
    widgets_clear(s);

    static const float coords[] = {
        -1.0f, -1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 0.0f,
    };

    s->coords = ngpu_buffer_create(gpu_ctx);
    if (!s->coords)
        return NGL_ERROR_MEMORY;


    ret = ngpu_buffer_init(s->coords, sizeof(coords), NGPU_BUFFER_USAGE_DYNAMIC_BIT |
                                                          NGPU_BUFFER_USAGE_TRANSFER_DST_BIT |
                                                          NGPU_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (ret < 0)
        return ret;

    ret = ngpu_buffer_upload(s->coords, coords, 0, sizeof(coords));
    if (ret < 0)
        return ret;

    struct ngpu_texture_params tex_params = {
        .type          = NGPU_TEXTURE_TYPE_2D,
        .format        = NGPU_FORMAT_R8G8B8A8_UNORM,
        .width         = (uint32_t)s->canvas.w,
        .height        = (uint32_t)s->canvas.h,
        .min_filter    = NGPU_FILTER_NEAREST,
        .mag_filter    = NGPU_FILTER_NEAREST,
        .usage         = NGPU_TEXTURE_USAGE_TRANSFER_DST_BIT | NGPU_TEXTURE_USAGE_SAMPLED_BIT,
    };
    s->texture = ngpu_texture_create(gpu_ctx);
    if (!s->texture)
        return NGL_ERROR_MEMORY;
    ret = ngpu_texture_init(s->texture, &tex_params);
    if (ret < 0)
        return ret;

    struct ngpu_block_desc transforms_block_desc;
    ngpu_block_desc_init(gpu_ctx, &transforms_block_desc, NGPU_BLOCK_LAYOUT_STD140);
    ngpu_block_desc_add_field(&transforms_block_desc, "modelview_matrix", NGPU_TYPE_MAT4, 0);
    ngpu_block_desc_add_field(&transforms_block_desc, "projection_matrix", NGPU_TYPE_MAT4, 0);
    ngli_assert(ngpu_block_desc_get_size(&transforms_block_desc, 0) == sizeof(struct transforms_block));

    struct ngpu_buffer *staging_buf = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);

    const struct ngpu_pgcraft_block blocks[] = {
        {
            .name          = "transforms",
            .instance_name = "",
            .type          = NGPU_TYPE_UNIFORM_BUFFER,
            .stage         = NGPU_PROGRAM_STAGE_VERT,
            .block         = &transforms_block_desc,
            .buffer = {
                .buffer = staging_buf,
                .size   = sizeof(struct transforms_block),
            }
        },
    };

    struct ngpu_pgcraft_texture textures[] = {
        {
            .name        = "tex",
            .type        = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
            .stage       = NGPU_PROGRAM_STAGE_FRAG,
            .texture     = s->texture,
            .no_metadata = true,
        },
    };

    const struct ngpu_pgcraft_attribute attributes[] = {
        {
            .name     = "coords",
            .type     = NGPU_TYPE_VEC4,
            .format   = NGPU_FORMAT_R32G32B32A32_SFLOAT,
            .stride   = 4 * sizeof(float),
            .buffer   = s->coords,
        },
    };

    struct ngpu_graphics_state graphics_state = NGPU_GRAPHICS_STATE_DEFAULTS;
    graphics_state.blend = 1;
    graphics_state.blend_src_factor = NGPU_BLEND_FACTOR_SRC_ALPHA;
    graphics_state.blend_dst_factor = NGPU_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    graphics_state.blend_src_factor_a = NGPU_BLEND_FACTOR_ZERO;
    graphics_state.blend_dst_factor_a = NGPU_BLEND_FACTOR_ONE;

    const struct ngpu_pgcraft_params crafter_params = {
        .program_label    = "nopegl/hud",
        .vert_base        = vertex_data,
        .frag_base        = fragment_data,
        .blocks           = blocks,
        .nb_blocks        = NGLI_ARRAY_NB(blocks),
        .textures         = textures,
        .nb_textures      = NGLI_ARRAY_NB(textures),
        .attributes       = attributes,
        .nb_attributes    = NGLI_ARRAY_NB(attributes),
        .vert_out_vars    = vert_out_vars,
        .nb_vert_out_vars = NGLI_ARRAY_NB(vert_out_vars),
    };

    s->crafter = ngpu_pgcraft_create(gpu_ctx);
    if (!s->crafter) {
        ret = NGL_ERROR_MEMORY;
        goto done;
    }

    ret = ngpu_pgcraft_craft(s->crafter, &crafter_params);
    if (ret < 0)
        goto done;

    s->transforms_block_index = ngpu_pgcraft_get_block_index(s->crafter, "transforms", NGPU_PROGRAM_STAGE_VERT);

    s->pipeline_compat = ngli_pipeline_compat_create(gpu_ctx);
    if (!s->pipeline_compat) {
        ret = NGL_ERROR_MEMORY;
        goto done;
    }

    const struct pipeline_compat_params params = {
        .type         = NGPU_PIPELINE_TYPE_GRAPHICS,
        .graphics     = {
            .topology = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            .state    = graphics_state,
            .rt_layout    = ctx->default_rendertarget_layout,
            .vertex_state = ngpu_pgcraft_get_vertex_state(s->crafter),
        },
        .program          = ngpu_pgcraft_get_program(s->crafter),
        .layout_desc      = ngpu_pgcraft_get_bindgroup_layout_desc(s->crafter),
        .resources        = ngpu_pgcraft_get_bindgroup_resources(s->crafter),
        .vertex_resources = ngpu_pgcraft_get_vertex_resources(s->crafter),
        .texture_infos    = ngpu_pgcraft_get_texture_infos(s->crafter),
    };

    ret = ngli_pipeline_compat_init(s->pipeline_compat, &params);

done:
    ngpu_block_desc_reset(&transforms_block_desc);
    return ret;
}

void ngli_hud_draw(struct hud *s, const struct ngli_frame_stats *stats)
{
    struct ngl_ctx *ctx = s->ctx;
    struct ngpu_ctx *gpu_ctx = ctx->gpu_ctx;

    s->frame_stats = *stats;
    widgets_make_stats(s);
    if (s->export_filename) {
        widgets_csv_report(s);
        return;
    }

    const double t = (double)ngli_gettime_relative() / 1000000.;
    const int need_refresh = fabs(t - s->last_refresh_time) >= s->refresh_rate_interval;
    if (need_refresh) {
        s->last_refresh_time = t;
        widgets_clear(s);
        widgets_draw(s);
    }

    const struct ngpu_viewport viewport = {
        .x      = truncf(ctx->viewport.x),
        .y      = truncf(ctx->viewport.y),
        .width  = truncf(ctx->viewport.width),
        .height = truncf(ctx->viewport.height),
    };
    if (!ngpu_viewport_is_valid(&viewport))
        return;

    const int int_ratio_w = (int)viewport.width  / s->canvas.w;
    const int int_ratio_h = (int)viewport.height / s->canvas.h;
    const int max_scale = NGLI_MIN(int_ratio_w, int_ratio_h);
    const int scale = NGLI_CLAMP(s->scale > 0 ? s->scale : 1, 1, max_scale);
    const float ratio_w = (float)(scale * s->canvas.w) / viewport.width;
    const float ratio_h = (float)(scale * s->canvas.h) / viewport.height;
    const float x =-1.0f + 2 * ratio_w;
    const float y = 1.0f - 2 * ratio_h;
    const float coords[] = {
        -1.0f,  y,    0.0f, 1.0f,
         x,     y,    1.0f, 1.0f,
        -1.0f,  1.0f, 0.0f, 0.0f,
         x,     1.0f, 1.0f, 0.0f,
    };

    int ret = ngpu_buffer_upload(s->coords, coords, 0, sizeof(coords));
    if (ret < 0)
        return;

    ret = ngpu_texture_upload(s->texture, s->canvas.buf, 0);
    if (ret < 0)
        return;

    if (!ngpu_ctx_is_render_pass_active(gpu_ctx)) {
        ngpu_ctx_begin_render_pass(gpu_ctx, ctx->current_rendertarget);
    }

    ngpu_ctx_set_viewport(gpu_ctx, &viewport);
    ngpu_ctx_set_scissor(gpu_ctx, &ctx->scissor);

    const struct ngli_mat4 *modelview_matrix = ngli_darray_tail(&ctx->modelview_matrix_stack);
    const struct ngli_mat4 *projection_matrix = ngli_darray_tail(&ctx->projection_matrix_stack);
    struct transforms_block transforms_data = {0};
    transforms_data.modelview_matrix = *modelview_matrix;
    transforms_data.projection_matrix = *projection_matrix;

    const size_t offset = ngpu_staging_buffer_push(ctx->current_staging_buffer, &transforms_data, sizeof(transforms_data));
    struct ngpu_buffer *buffer = ngpu_staging_buffer_get_buffer(ctx->current_staging_buffer);
    ngli_pipeline_compat_update_buffer(s->pipeline_compat, s->transforms_block_index, buffer, offset, sizeof(transforms_data));

    ngli_pipeline_compat_draw(s->pipeline_compat, 4, 1, 0);
}

void ngli_hud_freep(struct hud **sp)
{
    struct hud *s = *sp;
    if (!s)
        return;

    ngli_pipeline_compat_freep(&s->pipeline_compat);
    ngpu_pgcraft_freep(&s->crafter);
    ngpu_texture_freep(&s->texture);
    ngpu_buffer_freep(&s->coords);
    widgets_uninit(s);
    ngli_free(s->canvas.buf);
    if (s->fp_export) {
        fclose(s->fp_export);
        ngli_bstr_freep(&s->csv_line);
    }

    ngli_freep(sp);
}
