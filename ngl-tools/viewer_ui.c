/*
 * Copyright 2025-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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

#include <Python.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nuklear_ngpu.h"
#include "viewer.h"
#include "viewer_ui.h"

struct edit_param_arg {
    struct ngl_node *node;
    char key[64];
    union {
        int      i;
        unsigned u;
        float    f;
        double   d;
        float    v[4];
        char     s[256];
    } value;
};

static void edit_arg_free(void *arg) { free(arg); }

#define DEFINE_EDIT_CB(name, body)                                                 \
    static void name(struct viewer_ctx *s, void *arg)                              \
    {                                                                              \
        (void)s;                                                                   \
        struct edit_param_arg *e = arg;                                            \
        body;                                                                      \
    }

DEFINE_EDIT_CB(edit_cb_bool,   ngl_node_param_set_bool  (e->node, e->key, e->value.i))
DEFINE_EDIT_CB(edit_cb_i32,    ngl_node_param_set_i32   (e->node, e->key, (int32_t)e->value.i))
DEFINE_EDIT_CB(edit_cb_u32,    ngl_node_param_set_u32   (e->node, e->key, e->value.u))
DEFINE_EDIT_CB(edit_cb_f32,    ngl_node_param_set_f32   (e->node, e->key, e->value.f))
DEFINE_EDIT_CB(edit_cb_f64,    ngl_node_param_set_f64   (e->node, e->key, e->value.d))
DEFINE_EDIT_CB(edit_cb_vec2,   ngl_node_param_set_vec2  (e->node, e->key, e->value.v))
DEFINE_EDIT_CB(edit_cb_vec3,   ngl_node_param_set_vec3  (e->node, e->key, e->value.v))
DEFINE_EDIT_CB(edit_cb_vec4,   ngl_node_param_set_vec4  (e->node, e->key, e->value.v))
DEFINE_EDIT_CB(edit_cb_select, ngl_node_param_set_select(e->node, e->key, e->value.s))

static struct edit_param_arg *edit_arg_new(struct ngl_node *node, const char *key)
{
    struct edit_param_arg *e = calloc(1, sizeof(*e));
    if (!e)
        return NULL;
    e->node = node;
    snprintf(e->key, sizeof(e->key), "%s", key);
    return e;
}

static void post_edit(struct viewer_ctx *s, void (*fn)(struct viewer_ctx *, void *), void *arg)
{
    scene_cmd_post(&s->cmd_q, (struct scene_cmd){
        .type = SCENE_CMD_CALLBACK,
        .callback = { .fn = fn, .free_fn = edit_arg_free, .arg = arg },
    });
}

#define POST_EDIT(s, fn, node, key, init)                                          \
    do {                                                                           \
        struct edit_param_arg *e_ = edit_arg_new((node), (key));                   \
        if (e_) { init; post_edit((s), (fn), e_); }                                \
    } while (0)

static int param_is_editable(const struct ngl_param_info *p, struct ngl_node *node)
{
    if (!p->allow_live_change)
        return 0;
    if (p->allow_node) {
        struct ngl_node *child_node = NULL;
        if (ngl_node_param_get_node(node, p->key, &child_node) == 0 && child_node)
            return 0;
    }
    return 1;
}

static void property_row(struct nk_context *nk, float K, float h, int cells)
{
    nk_layout_row_template_begin(nk, h);
    nk_layout_row_template_push_static(nk, 8.f * K);
    for (int i = 0; i < cells; i++)
        nk_layout_row_template_push_dynamic(nk);
    nk_layout_row_template_end(nk);
    nk_spacing(nk, 1);
}

static void render_param_widget(struct viewer_ctx *s, struct nk_context *nk,
                                struct ngl_node *node, const struct ngl_param_info *p)
{
    const float K = viewer_ui_scale(s);
    const int live = param_is_editable(p, node);

    /* Color-like params: any vec3/vec4 whose key contains "color". */
    const bool is_color_vec = p->key && strstr(p->key, "color")
                             && (p->type == NGL_PARAM_TYPE_VEC3 || p->type == NGL_PARAM_TYPE_VEC4);
    if (is_color_vec) {
        const int n = (p->type == NGL_PARAM_TYPE_VEC3) ? 3 : 4;
        float v[4] = {0, 0, 0, 1.0f};
        if (n == 3) ngl_node_param_get_vec3(node, p->key, v);
        else        ngl_node_param_get_vec4(node, p->key, v);

        property_row(nk, K, 22 * K, 2);
        nk_label(nk, p->key, NK_TEXT_LEFT);

        const struct nk_colorf cf = { v[0], v[1], v[2], v[3] };
        const struct nk_color swatch = nk_rgba_cf(cf);

        if (!live) {
            nk_button_color(nk, swatch);
            return;
        }

        if (nk_combo_begin_color(nk, swatch, nk_vec2(nk_widget_width(nk), 320 * K))) {
            const enum nk_color_format fmt = (n == 4) ? NK_RGBA : NK_RGB;
            nk_layout_row_dynamic(nk, 180 * K, 1);
            struct nk_colorf new_cf = nk_color_picker(nk, cf, fmt);
            nk_layout_row_dynamic(nk, 22 * K, 1);
            new_cf.r = nk_propertyf(nk, "#R", 0.0f, new_cf.r, 1.0f, 0.01f, 0.005f);
            new_cf.g = nk_propertyf(nk, "#G", 0.0f, new_cf.g, 1.0f, 0.01f, 0.005f);
            new_cf.b = nk_propertyf(nk, "#B", 0.0f, new_cf.b, 1.0f, 0.01f, 0.005f);
            if (n == 4)
                new_cf.a = nk_propertyf(nk, "#A", 0.0f, new_cf.a, 1.0f, 0.01f, 0.005f);
            nk_combo_end(nk);

            float nv[4] = { new_cf.r, new_cf.g, new_cf.b, (n == 4) ? new_cf.a : 1.0f };
            if (memcmp(nv, v, (size_t)n * sizeof(float)) != 0) {
                void (*fn)(struct viewer_ctx *, void *) = (n == 3) ? edit_cb_vec3 : edit_cb_vec4;
                POST_EDIT(s, fn, node, p->key, memcpy(e_->value.v, nv, sizeof(nv)));
            }
        }
        return;
    }

    switch (p->type) {
    case NGL_PARAM_TYPE_BOOL: {
        int v = 0;
        ngl_node_param_get_bool(node, p->key, &v);
        property_row(nk, K, 22 * K, 2);
        nk_label(nk, p->key, NK_TEXT_LEFT);
        if (live) {
            int new_v = v;
            nk_checkbox_label(nk, "", &new_v);
            if (new_v != v)
                POST_EDIT(s, edit_cb_bool, node, p->key, e_->value.i = new_v);
        } else {
            nk_label(nk, v ? "true" : "false", NK_TEXT_LEFT);
        }
        break;
    }
    case NGL_PARAM_TYPE_I32: {
        int32_t v = 0;
        ngl_node_param_get_i32(node, p->key, &v);
        property_row(nk, K, 22 * K, 2);
        nk_label(nk, p->key, NK_TEXT_LEFT);
        if (live) {
            int new_v = nk_propertyi(nk, "#", -1000000, v, 1000000, 1, 1.0f);
            if (new_v != v)
                POST_EDIT(s, edit_cb_i32, node, p->key, e_->value.i = new_v);
        } else {
            char buf[32]; snprintf(buf, sizeof(buf), "%d", v);
            nk_label(nk, buf, NK_TEXT_LEFT);
        }
        break;
    }
    case NGL_PARAM_TYPE_U32: {
        uint32_t v = 0;
        ngl_node_param_get_u32(node, p->key, &v);
        property_row(nk, K, 22 * K, 2);
        nk_label(nk, p->key, NK_TEXT_LEFT);
        if (live) {
            int new_v = nk_propertyi(nk, "#", 0, (int)v, 1000000, 1, 1.0f);
            if ((unsigned)new_v != v)
                POST_EDIT(s, edit_cb_u32, node, p->key, e_->value.u = (unsigned)new_v);
        } else {
            char buf[32]; snprintf(buf, sizeof(buf), "%u", v);
            nk_label(nk, buf, NK_TEXT_LEFT);
        }
        break;
    }
    case NGL_PARAM_TYPE_F32: {
        float v = 0.0f;
        ngl_node_param_get_f32(node, p->key, &v);
        property_row(nk, K, 22 * K, 2);
        nk_label(nk, p->key, NK_TEXT_LEFT);
        if (live) {
            float new_v = nk_propertyf(nk, "#", -1e6f, v, 1e6f, 0.01f, 0.005f);
            if (new_v != v)
                POST_EDIT(s, edit_cb_f32, node, p->key, e_->value.f = new_v);
        } else {
            char buf[32]; snprintf(buf, sizeof(buf), "%.4f", (double)v);
            nk_label(nk, buf, NK_TEXT_LEFT);
        }
        break;
    }
    case NGL_PARAM_TYPE_F64: {
        double v = 0.0;
        ngl_node_param_get_f64(node, p->key, &v);
        property_row(nk, K, 22 * K, 2);
        nk_label(nk, p->key, NK_TEXT_LEFT);
        if (live) {
            double new_v = (double)nk_propertyf(nk, "#", -1e6f, (float)v, 1e6f, 0.01f, 0.005f);
            if (new_v != v)
                POST_EDIT(s, edit_cb_f64, node, p->key, e_->value.d = new_v);
        } else {
            char buf[32]; snprintf(buf, sizeof(buf), "%.4f", v);
            nk_label(nk, buf, NK_TEXT_LEFT);
        }
        break;
    }
    case NGL_PARAM_TYPE_VEC2:
    case NGL_PARAM_TYPE_VEC3:
    case NGL_PARAM_TYPE_VEC4: {
        const int n = p->type == NGL_PARAM_TYPE_VEC2 ? 2 : p->type == NGL_PARAM_TYPE_VEC3 ? 3 : 4;
        float v[4] = {0};
        if (n == 2) ngl_node_param_get_vec2(node, p->key, v);
        else if (n == 3) ngl_node_param_get_vec3(node, p->key, v);
        else ngl_node_param_get_vec4(node, p->key, v);
        property_row(nk, K, 18 * K, 1);
        nk_label(nk, p->key, NK_TEXT_LEFT);
        property_row(nk, K, 22 * K, n);
        float nv[4]; memcpy(nv, v, sizeof(nv));
        for (int j = 0; j < n; j++) {
            char id_j[8];
            const char axis = "XYZW"[j];
            snprintf(id_j, sizeof(id_j), "#%c", axis);
            if (live)
                nv[j] = nk_propertyf(nk, id_j, -1e6f, nv[j], 1e6f, 0.01f, 0.005f);
            else {
                char buf[32]; snprintf(buf, sizeof(buf), "%.4f", (double)nv[j]);
                nk_label(nk, buf, NK_TEXT_RIGHT);
            }
        }
        if (live && memcmp(v, nv, (size_t)n * sizeof(float)) != 0) {
            void (*fn)(struct viewer_ctx *, void *) =
                n == 2 ? edit_cb_vec2 : n == 3 ? edit_cb_vec3 : edit_cb_vec4;
            POST_EDIT(s, fn, node, p->key, memcpy(e_->value.v, nv, sizeof(nv)));
        }
        break;
    }
    case NGL_PARAM_TYPE_SELECT: {
        const char *cur = NULL;
        ngl_node_param_get_select(node, p->key, &cur);
        property_row(nk, K, 22 * K, 2);
        nk_label(nk, p->key, NK_TEXT_LEFT);
        if (live && p->choices && cur) {
            if (nk_combo_begin_label(nk, cur, nk_vec2(nk_widget_width(nk), 200 * K))) {
                nk_layout_row_dynamic(nk, 22 * K, 1);
                for (size_t i = 0; p->choices[i]; i++) {
                    if (nk_combo_item_label(nk, p->choices[i], NK_TEXT_LEFT) && strcmp(p->choices[i], cur) != 0)
                        POST_EDIT(s, edit_cb_select, node, p->key,
                                  snprintf(e_->value.s, sizeof(e_->value.s), "%s", p->choices[i]));
                }
                nk_combo_end(nk);
            }
        } else {
            nk_label(nk, cur ? cur : "(none)", NK_TEXT_LEFT);
        }
        break;
    }
    case NGL_PARAM_TYPE_STR: {
        const char *v = NULL;
        ngl_node_param_get_str(node, p->key, &v);
        property_row(nk, K, 22 * K, 2);
        nk_label(nk, p->key, NK_TEXT_LEFT);
        nk_label(nk, v ? v : "(null)", NK_TEXT_LEFT);
        break;
    }
    case NGL_PARAM_TYPE_RATIONAL: {
        int32_t num = 0, den = 1;
        ngl_node_param_get_rational(node, p->key, &num, &den);
        property_row(nk, K, 22 * K, 2);
        nk_label(nk, p->key, NK_TEXT_LEFT);
        char buf[32]; snprintf(buf, sizeof(buf), "%d/%d", num, den);
        nk_label(nk, buf, NK_TEXT_LEFT);
        break;
    }
    default:
        break;
    }
}

static int param_has_widget(const struct ngl_param_info *p, struct ngl_node *node)
{
    if (p->allow_node) {
        struct ngl_node *child_node = NULL;
        if (ngl_node_param_get_node(node, p->key, &child_node) == 0 && child_node)
            return 0;
    }
    switch (p->type) {
    case NGL_PARAM_TYPE_BOOL:
    case NGL_PARAM_TYPE_I32:
    case NGL_PARAM_TYPE_U32:
    case NGL_PARAM_TYPE_F32:
    case NGL_PARAM_TYPE_F64:
    case NGL_PARAM_TYPE_VEC2:
    case NGL_PARAM_TYPE_VEC3:
    case NGL_PARAM_TYPE_VEC4:
    case NGL_PARAM_TYPE_SELECT:
    case NGL_PARAM_TYPE_STR:
    case NGL_PARAM_TYPE_RATIONAL:
        return 1;
    default:
        return 0;
    }
}

static void viewer_set_selected_node(struct viewer_ctx *s, struct ngl_node *node)
{
    if (s->selected_node == node)
        return;
    if (s->selected_node)
        ngl_node_unrefp(&s->selected_node);
    if (node)
        s->selected_node = ngl_node_ref(node);
}

static void render_node_tree(struct viewer_ctx *s, struct nk_context *nk,
                             struct ngl_node *node, int depth)
{
    if (!node || depth > 32)
        return;

    const char *type_name = ngl_node_get_type_name(node);
    const char *label = NULL;
    ngl_node_get_label(node, &label);

    char display[256];
    if (label && label[0] && type_name && strcmp(label, type_name) != 0)
        snprintf(display, sizeof(display), "%s (%s)", type_name, label);
    else
        snprintf(display, sizeof(display), "%s", type_name ? type_name : "?");

    nk_bool selected = (s->selected_node == node);
    const nk_bool was_selected = selected;
    const int open = nk_tree_element_push_id(nk, NK_TREE_NODE, display,
                                             depth == 0 ? NK_MAXIMIZED : NK_MINIMIZED,
                                             &selected, (int)(uintptr_t)node);
    if (selected != was_selected) {
        if (selected)
            viewer_set_selected_node(s, node);
        else if (s->selected_node == node)
            viewer_set_selected_node(s, NULL);
    }
    if (!open)
        return;

    struct ngl_node **children = NULL;
    size_t nb_children = 0;
    ngl_node_get_children(node, &children, &nb_children);
    for (size_t i = 0; i < nb_children; i++)
        render_node_tree(s, nk, children[i], depth + 1);
    ngl_node_children_freep(&children);

    nk_tree_element_pop(nk);
}

static void render_scene_graph_section(struct viewer_ctx *s, struct nk_context *nk, float K)
{
    if (!s->scene_loaded)
        return;
    nk_layout_row_dynamic(nk, 10 * K, 1);
    nk_spacing(nk, 1);
    if (!nk_tree_push(nk, NK_TREE_TAB, "Scene Graph", NK_MINIMIZED))
        return;
    struct ngl_node *root = ngl_get_scene_root(s->ngl_ctx);
    if (root) {
        const struct nk_vec2 saved_tab_padding = nk->style.tab.padding;
        nk->style.tab.padding.y = 0.0f;
        render_node_tree(s, nk, root, 0);
        nk->style.tab.padding = saved_tab_padding;
    } else {
        nk_layout_row_dynamic(nk, 22 * K, 1);
        nk_label(nk, "(no scene)", NK_TEXT_LEFT);
    }
    nk_tree_pop(nk);
}

static void render_properties_panel_body(struct viewer_ctx *s, struct nk_context *nk, float K)
{
    struct ngl_node *node = s->selected_node;
    if (!node)
        return;

    const char *type_name = ngl_node_get_type_name(node);
    const char *label = NULL;
    ngl_node_get_label(node, &label);

    char header[256];
    if (label && label[0] && type_name && strcmp(label, type_name) != 0)
        snprintf(header, sizeof(header), "%s (%s)", type_name, label);
    else
        snprintf(header, sizeof(header), "%s", type_name ? type_name : "?");

    nk_layout_row_dynamic(nk, 22 * K, 1);
    nk_label(nk, header, NK_TEXT_LEFT);

    const struct ngl_param_info *infos = NULL;
    size_t nb_infos = 0;
    ngl_node_get_params_info(node, &infos, &nb_infos);
    bool has_widget = false;
    for (size_t i = 0; i < nb_infos; i++) {
        if (param_has_widget(&infos[i], node)) {
            render_param_widget(s, nk, node, &infos[i]);
            has_widget = true;
        }
    }
    ngl_node_params_info_freep(&infos, nb_infos);

    if (!has_widget) {
        nk_layout_row_dynamic(nk, 22 * K, 1);
        nk_label(nk, "(no parameters to display)", NK_TEXT_LEFT);
    }
}

/*
 * Fill an export_params from the current UI state. The export worker
 * fetches the scene itself via snapshot_queue and derives width from the
 * snapshot's intrinsic aspect.
 */
static struct export_params make_export_params(struct viewer_ctx *s)
{
    return (struct export_params){
        .filename       = s->export_path,
        .snapshot_queue = &s->cmd_q,
        .profile_index  = s->export_profile,
        .height         = (uint32_t)s->export_height,
        .framerate      = {s->framerate[0], s->framerate[1]},
        .duration       = s->duration,
    };
}

static void SDLCALL file_dialog_callback(void *userdata, const char *const *filelist, int filter)
{
    struct viewer_ctx *s = userdata;
    if (!filelist || !filelist[0])
        return;
    viewer_load_script(s, filelist[0]);
}

static void SDLCALL save_dialog_callback(void *userdata, const char *const *filelist, int filter)
{
    struct viewer_ctx *s = userdata;
    if (filelist && filelist[0])
        snprintf(s->export_path, sizeof(s->export_path), "%s", filelist[0]);
}

#define NK_SDL_DOUBLE_CLICK_LO 0.02
#define NK_SDL_DOUBLE_CLICK_HI 0.2

static void nk_sdl_update_text_input(struct viewer_ctx *s)
{
    struct nk_context *ctx = nk_ngpu_get_nk_ctx(s->nk_ngpu_ctx);
    int active;
    if (!ctx->active)
        active = 0;
    else if (ctx->active->popup.win)
        active = ctx->active->popup.win->edit.active;
    else
        active = ctx->active->edit.active;

    if (active != s->nk_edit_was_active) {
        if (active)
            SDL_StartTextInput(s->window);
        else
            SDL_StopTextInput(s->window);
        s->nk_edit_was_active = active;
    }
}

void nk_sdl_input(struct viewer_ctx *s, SDL_Event *evt)
{
    struct nk_context *nk = nk_ngpu_get_nk_ctx(s->nk_ngpu_ctx);
    const float scale = s->dpi_scale;
    /*
     * SDL3 reports event coordinates in logical (point) units even with
     * SDL_WINDOW_HIGH_PIXEL_DENSITY. Nuklear lays out in physical pixels so
     * every position from SDL3 needs to be multiplied by the display scale
     * before being submitted to Nuklear.
     */
    switch (evt->type) {
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_KEY_DOWN: {
        const int down = evt->type == SDL_EVENT_KEY_DOWN;
        const int ctrl = evt->key.mod & (SDL_KMOD_LCTRL | SDL_KMOD_RCTRL);
        switch (evt->key.scancode) {
        case SDL_SCANCODE_RSHIFT:
        case SDL_SCANCODE_LSHIFT:    nk_input_key(nk, NK_KEY_SHIFT, down); break;
        case SDL_SCANCODE_DELETE:    nk_input_key(nk, NK_KEY_DEL, down); break;
        case SDL_SCANCODE_RETURN:    nk_input_key(nk, NK_KEY_ENTER, down); break;
        case SDL_SCANCODE_TAB:       nk_input_key(nk, NK_KEY_TAB, down); break;
        case SDL_SCANCODE_BACKSPACE: nk_input_key(nk, NK_KEY_BACKSPACE, down); break;
        case SDL_SCANCODE_HOME:      nk_input_key(nk, NK_KEY_TEXT_START, down);
                                     nk_input_key(nk, NK_KEY_SCROLL_START, down); break;
        case SDL_SCANCODE_END:       nk_input_key(nk, NK_KEY_TEXT_END, down);
                                     nk_input_key(nk, NK_KEY_SCROLL_END, down); break;
        case SDL_SCANCODE_PAGEDOWN:  nk_input_key(nk, NK_KEY_SCROLL_DOWN, down); break;
        case SDL_SCANCODE_PAGEUP:    nk_input_key(nk, NK_KEY_SCROLL_UP, down); break;
        case SDL_SCANCODE_A:         nk_input_key(nk, NK_KEY_TEXT_SELECT_ALL, down && ctrl); break;
        case SDL_SCANCODE_Z:         nk_input_key(nk, NK_KEY_TEXT_UNDO, down && ctrl); break;
        case SDL_SCANCODE_R:         nk_input_key(nk, NK_KEY_TEXT_REDO, down && ctrl); break;
        case SDL_SCANCODE_C:         nk_input_key(nk, NK_KEY_COPY, down && ctrl); break;
        case SDL_SCANCODE_V:         nk_input_key(nk, NK_KEY_PASTE, down && ctrl); break;
        case SDL_SCANCODE_X:         nk_input_key(nk, NK_KEY_CUT, down && ctrl); break;
        case SDL_SCANCODE_B:         nk_input_key(nk, NK_KEY_TEXT_LINE_START, down && ctrl); break;
        case SDL_SCANCODE_E:         nk_input_key(nk, NK_KEY_TEXT_LINE_END, down && ctrl); break;
        case SDL_SCANCODE_UP:        nk_input_key(nk, NK_KEY_UP, down); break;
        case SDL_SCANCODE_DOWN:      nk_input_key(nk, NK_KEY_DOWN, down); break;
        case SDL_SCANCODE_ESCAPE:    nk_input_key(nk, NK_KEY_TEXT_RESET_MODE, down); break;
        case SDL_SCANCODE_LEFT:
            nk_input_key(nk, ctrl ? NK_KEY_TEXT_WORD_LEFT : NK_KEY_LEFT, down);
            break;
        case SDL_SCANCODE_RIGHT:
            nk_input_key(nk, ctrl ? NK_KEY_TEXT_WORD_RIGHT : NK_KEY_RIGHT, down);
            break;
        default:
            break;
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const int x = (int)(evt->button.x * scale);
        const int y = (int)(evt->button.y * scale);
        const int down = evt->button.down;
        switch (evt->button.button) {
        case SDL_BUTTON_LEFT: {
            const double dt = (double)(evt->button.timestamp - s->nk_last_left_click_ts) / 1000000000.0;
            nk_input_button(nk, NK_BUTTON_LEFT, x, y, down);
            nk_input_button(nk, NK_BUTTON_DOUBLE, x, y,
                            down && dt > NK_SDL_DOUBLE_CLICK_LO && dt < NK_SDL_DOUBLE_CLICK_HI);
            s->nk_last_left_click_ts = evt->button.timestamp;
            break;
        }
        case SDL_BUTTON_MIDDLE: nk_input_button(nk, NK_BUTTON_MIDDLE, x, y, down); break;
        case SDL_BUTTON_RIGHT:  nk_input_button(nk, NK_BUTTON_RIGHT, x, y, down); break;
        default: break;
        }
        break;
    }
    case SDL_EVENT_MOUSE_MOTION:
        nk->input.mouse.pos.x = evt->motion.x * scale;
        nk->input.mouse.pos.y = evt->motion.y * scale;
        nk->input.mouse.delta.x = nk->input.mouse.pos.x - nk->input.mouse.prev.x;
        nk->input.mouse.delta.y = nk->input.mouse.pos.y - nk->input.mouse.prev.y;
        break;
    case SDL_EVENT_MOUSE_WHEEL:
        nk_input_scroll(nk, nk_vec2(evt->wheel.x, evt->wheel.y));
        break;
    case SDL_EVENT_TEXT_INPUT: {
        nk_glyph glyph;
        size_t len = strlen(evt->text.text);
        if (len <= NK_UTF_SIZE) {
            memcpy(glyph, evt->text.text, len);
            nk_input_glyph(nk, glyph);
        }
        break;
    }
    default:
        break;
    }
}

void viewer_hooks_refresh(struct viewer_ctx *s)
{
    if (!s->hooks)
        return;

    char **disabled_ids = NULL;
    size_t nb_disabled = 0;
    for (size_t i = 0; i < s->nb_hooks_sessions; i++) {
        if (!s->hooks_session_enabled || s->hooks_session_enabled[i])
            continue;
        const char *id = s->hooks_sessions[i].id;
        if (!id)
            continue;
        char **tmp = SDL_realloc(disabled_ids, (nb_disabled + 1) * sizeof(*disabled_ids));
        if (!tmp)
            break;
        disabled_ids = tmp;
        disabled_ids[nb_disabled] = SDL_strdup(id);
        if (!disabled_ids[nb_disabled])
            break;
        nb_disabled++;
    }

    PyGILState_STATE gstate = PyGILState_Ensure();
    hooks_free_sessions(s->hooks_sessions, s->nb_hooks_sessions);
    SDL_free(s->hooks_session_enabled);
    s->hooks_sessions = NULL;
    s->hooks_session_enabled = NULL;
    s->nb_hooks_sessions = 0;
    hooks_get_sessions(s->hooks, &s->hooks_sessions, &s->nb_hooks_sessions);
    PyGILState_Release(gstate);

    if (s->nb_hooks_sessions > 0) {
        s->hooks_session_enabled = SDL_calloc(s->nb_hooks_sessions, sizeof(int));
        if (s->hooks_session_enabled) {
            for (size_t i = 0; i < s->nb_hooks_sessions; i++) {
                s->hooks_session_enabled[i] = 1;
                for (size_t j = 0; j < nb_disabled; j++) {
                    if (s->hooks_sessions[i].id &&
                        !strcmp(disabled_ids[j], s->hooks_sessions[i].id)) {
                        s->hooks_session_enabled[i] = 0;
                        break;
                    }
                }
            }
        } else {
            /* Drop the sessions we just fetched so the UI loop, which keys
             * off nb_hooks_sessions, can't dereference a NULL enable array. */
            hooks_free_sessions(s->hooks_sessions, s->nb_hooks_sessions);
            s->hooks_sessions = NULL;
            s->nb_hooks_sessions = 0;
        }
    }

    for (size_t i = 0; i < nb_disabled; i++)
        SDL_free(disabled_ids[i]);
    SDL_free(disabled_ids);
}

void viewer_hooks_close(struct viewer_ctx *s)
{
    hooks_free_sessions(s->hooks_sessions, s->nb_hooks_sessions);
    SDL_free(s->hooks_session_enabled);
    s->hooks_sessions = NULL;
    s->hooks_session_enabled = NULL;
    s->nb_hooks_sessions = 0;
    if (s->hooks) {
        PyGILState_STATE gstate = PyGILState_Ensure();
        hooks_freep(&s->hooks);
        PyGILState_Release(gstate);
    }
}

void viewer_hooks_send_scene(struct viewer_ctx *s)
{
    if (!s->hooks || s->nb_hooks_sessions == 0 || !s->hooks_session_enabled)
        return;

    const char **ids = SDL_malloc(s->nb_hooks_sessions * sizeof(*ids));
    if (!ids)
        return;
    size_t nb_ids = 0;
    for (size_t i = 0; i < s->nb_hooks_sessions; i++)
        if (s->hooks_session_enabled[i])
            ids[nb_ids++] = s->hooks_sessions[i].id;
    if (nb_ids == 0) {
        SDL_free(ids);
        return;
    }

    SDL_Semaphore *sem = SDL_CreateSemaphore(0);
    if (!sem) {
        SDL_free(ids);
        return;
    }
    struct ngl_scene *snap = NULL;
    scene_cmd_post(&s->cmd_q, (struct scene_cmd){
        .type     = SCENE_CMD_SNAPSHOT,
        .snapshot = {.out = &snap, .done = sem},
    });
    SDL_WaitSemaphore(sem);
    SDL_DestroySemaphore(sem);

    if (snap) {
        PyGILState_STATE gstate = PyGILState_Ensure();
        for (size_t i = 0; i < nb_ids; i++) {
            struct ngl_scene *session_scene = (nb_ids == 1) ? snap : ngl_scene_duplicate(snap);
            if (!session_scene)
                continue;
            hooks_scene_change(s->hooks, ids[i], session_scene, 0x262626FF, 0);
            if (session_scene != snap)
                ngl_scene_unrefp(&session_scene);
        }
        PyGILState_Release(gstate);
        ngl_scene_unrefp(&snap);
    }
    SDL_free(ids);
}

static int format_time_label(char *buf, size_t size, double current, double total,
                             int32_t fr_num, int32_t fr_den)
{
    const int cm  = (int)(current / 60.0);
    const double cs_full = fmod(current, 60.0);
    const int cs  = (int)cs_full;
    const int cms = (int)((cs_full - (double)cs) * 1000.0);
    const int dm  = (int)(total / 60.0);
    const double ds_full = fmod(total, 60.0);
    const int ds  = (int)ds_full;
    const int dms = (int)((ds_full - (double)ds) * 1000.0);
    if (fr_num > 0 && fr_den > 0) {
        const int frame_idx = (int)(current * (double)fr_num / (double)fr_den + 0.5);
        return snprintf(buf, size, "%02d:%02d.%03d / %02d:%02d.%03d (%d @ %d/%d)",
                        cm, cs, cms, dm, ds, dms, frame_idx, fr_num, fr_den);
    }
    return snprintf(buf, size, "%02d:%02d.%03d / %02d:%02d.%03d",
                    cm, cs, cms, dm, ds, dms);
}

enum icon_kind {
    ICON_PLAY,
    ICON_PAUSE,
    ICON_STEP_BACK,
    ICON_STEP_FWD,
};

static int icon_button(struct nk_context *nk, float K, enum icon_kind kind, int enabled)
{
    struct nk_command_buffer *canvas = nk_window_get_canvas(nk);
    const struct nk_rect b = nk_widget_bounds(nk);
    if (!enabled)
        nk_widget_disable_begin(nk);
    const int clicked = nk_button_label(nk, "") && enabled;
    if (!enabled)
        nk_widget_disable_end(nk);

    /* Track input state at the widget bounds so the glyph color matches
     * the button background (normal/hover/active). Disabled buttons fall
     * back to a half-alpha normal color to read as grayed out. */
    struct nk_color c = nk->style.button.text_normal;
    if (enabled) {
        const int hovered = nk_input_is_mouse_hovering_rect(&nk->input, b);
        const int pressed = hovered && nk_input_is_mouse_down(&nk->input, NK_BUTTON_LEFT);
        c = pressed ? nk->style.button.text_active
            : hovered ? nk->style.button.text_hover
                      : nk->style.button.text_normal;
    } else {
        c.a = (nk_byte)((c.a + 1) / 2);
    }

    const float cx = b.x + b.w * 0.5f;
    const float cy = b.y + b.h * 0.5f;
    const float ih = b.h * 0.42f;       /* Icon visual height. */
    const float iw = ih * 0.85f;        /* Triangle width. */
    const float bar_w = NK_MAX(roundf(3.0f * K), 2.0f);
    const float gap = NK_MAX(roundf(3.0f * K), 2.0f);

    switch (kind) {
    case ICON_PLAY:
        nk_fill_triangle(canvas,
                         cx - iw * 0.5f, cy - ih * 0.5f,
                         cx - iw * 0.5f, cy + ih * 0.5f,
                         cx + iw * 0.5f, cy, c);
        break;
    case ICON_PAUSE: {
        const float pw = bar_w * 1.6f;  /* Pause bars read better when thicker. */
        nk_fill_rect(canvas, nk_rect(cx - gap * 0.5f - pw, cy - ih * 0.5f, pw, ih), 0.0f, c);
        nk_fill_rect(canvas, nk_rect(cx + gap * 0.5f,      cy - ih * 0.5f, pw, ih), 0.0f, c);
        break;
    }
    case ICON_STEP_BACK: {
        const float total = bar_w + gap + iw;
        const float bar_x = cx - total * 0.5f;
        nk_fill_rect(canvas, nk_rect(bar_x, cy - ih * 0.5f, bar_w, ih), 0.0f, c);
        const float tri_l = bar_x + bar_w + gap;
        nk_fill_triangle(canvas,
                         tri_l + iw, cy - ih * 0.5f,
                         tri_l + iw, cy + ih * 0.5f,
                         tri_l,      cy, c);
        break;
    }
    case ICON_STEP_FWD: {
        const float total = iw + gap + bar_w;
        const float tri_l = cx - total * 0.5f;
        nk_fill_triangle(canvas,
                         tri_l,      cy - ih * 0.5f,
                         tri_l,      cy + ih * 0.5f,
                         tri_l + iw, cy, c);
        const float bar_x = tri_l + iw + gap;
        nk_fill_rect(canvas, nk_rect(bar_x, cy - ih * 0.5f, bar_w, ih), 0.0f, c);
        break;
    }
    }
    return clicked;
}

void viewer_update_time(struct viewer_ctx *s)
{
    if (!s->paused) {
        if (s->clock_off < 0)
            s->clock_off = (int64_t)SDL_GetTicks();
        const int64_t now = (int64_t)SDL_GetTicks();
        double t = (double)(now - s->clock_off) / 1000.0;
        if (s->duration > 0.0 && t >= s->duration) {
            s->clock_off = now;
            t = 0.0;
        }
        viewer_set_frame_time(s, t);
    }
}

void viewer_ui(struct viewer_ctx *s)
{
    struct nk_context *nk = nk_ngpu_get_nk_ctx(s->nk_ngpu_ctx);
    /* Integer UI scale for layout (see viewer_ui_scale). Mouse coords
     * still use the real (fractional) dpi_scale in nk_sdl_input so clicks
     * map to where the user actually pointed. */
    const float K = viewer_ui_scale(s);

    /* Snapshot frame_time once for the whole UI frame: it's shared with the
     * scene thread, so all reads must go through the helper. */
    const double frame_time = viewer_get_frame_time(s);

    const float panel_width_now = (float)s->win_width * s->panel_ratio;
    const float split_hit_w = 8.0f * K;
    const struct nk_rect split_hit = nk_rect(panel_width_now - split_hit_w * 0.5f, 0.0f,
                                             split_hit_w, (float)s->win_height);
    const int mouse_down = nk->input.mouse.buttons[NK_BUTTON_LEFT].down;
    if (!mouse_down) {
        s->splitter_dragging = 0;
    } else if (!s->splitter_dragging
               && nk_input_has_mouse_click_in_rect(&nk->input, NK_BUTTON_LEFT, split_hit)) {
        s->splitter_dragging = 1;
    }
    if (s->splitter_dragging && s->win_width > 0) {
        const float new_ratio = nk->input.mouse.pos.x / (float)s->win_width;
        s->panel_ratio = NK_CLAMP(VIEWER_PANEL_RATIO_MIN, new_ratio, VIEWER_PANEL_RATIO_MAX);
    }

    /*
     * Switch cursor to east-west resize while hovering the hit zone or while
     * actively dragging; revert otherwise.
     */
    if (s->cursor_ew_resize) {
        const int splitter_hovered = nk_input_is_mouse_hovering_rect(&nk->input, split_hit);
        SDL_SetCursor((splitter_hovered || s->splitter_dragging)
                      ? s->cursor_ew_resize
                      : SDL_GetDefaultCursor());
    }

    const float margin = 4.0f * K;
    const float panel_width = (float)s->win_width * s->panel_ratio;
    const float controls_w = (float)panel_width - margin * 2;
    const float controls_h = (float)s->win_height - margin * 2;

    if (nk_begin(nk, "Controls", nk_rect(margin, margin, controls_w, controls_h),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE)) {

        /* Script section. */
        nk_layout_row_dynamic(nk, 25 * K, 1);
        nk_label(nk, s->script_path[0] ? s->script_path : "(drag & drop a .py script)", NK_TEXT_LEFT);

        nk_layout_row_dynamic(nk, 30 * K, 1);
        if (nk_button_label(nk, "Load Script...")) {
            static const SDL_DialogFileFilter filters[] = {
                {"Python scripts", "py"},
                {"All files", "*"},
            };
            const char *default_location = s->script_path[0] ? s->script_path : NULL;
            SDL_ShowOpenFileDialog(file_dialog_callback, s, s->window,
                                   filters, 2, default_location, 0);
        }

        if (s->nb_scenes > 0) {
            nk_layout_row_dynamic(nk, 25 * K, 1);
            const char *current = s->selected_scene >= 0
                ? s->scene_names[s->selected_scene]
                : "(select scene)";
            if (nk_combo_begin_label(nk, current, nk_vec2(nk_widget_width(nk), 200 * K))) {
                nk_layout_row_dynamic(nk, 25 * K, 1);
                for (size_t i = 0; i < s->nb_scenes; i++) {
                    if (nk_combo_item_label(nk, s->scene_names[i], NK_TEXT_LEFT)) {
                        if (s->selected_scene != (int)i) {
                            s->selected_scene = (int)i;
                            viewer_request_load(s);
                        }
                    }
                }
                nk_combo_end(nk);
            }
        }

        /* Export section. */
        nk_layout_row_dynamic(nk, 10 * K, 1);
        nk_spacing(nk, 1);

        if (nk_tree_push(nk, NK_TREE_TAB, "Export", NK_MINIMIZED)) {
            if (s->nb_avail_profiles == 0) {
                nk_layout_row_dynamic(nk, 25 * K, 1);
                nk_label(nk, "No encoders available, export disabled.", NK_TEXT_LEFT);
                nk_tree_pop(nk);
                goto export_section_done;
            }

            static const int heights[] = {360, 480, 720, 1080, 1440, 2160};
            static const char *height_labels[] = {"360p", "480p", "720p", "1080p", "1440p", "4K"};

            /* Profile selector. */
            nk_layout_row_dynamic(nk, 25 * K, 1);
            if (nk_combo_begin_label(nk, export_profiles[s->export_profile].name,
                                     nk_vec2(nk_widget_width(nk), 200 * K))) {
                nk_layout_row_dynamic(nk, 25 * K, 1);
                for (int i = 0; i < s->nb_avail_profiles; i++) {
                    const int real_idx = s->avail_profiles[i];
                    if (nk_combo_item_label(nk, export_profiles[real_idx].name, NK_TEXT_LEFT))
                        s->export_profile = real_idx;
                }
                nk_combo_end(nk);
            }

            /* Resolution selector. */
            nk_layout_row_dynamic(nk, 25 * K, 1);
            const char *cur_height = "720p";
            for (int i = 0; i < 6; i++)
                if (heights[i] == s->export_height)
                    cur_height = height_labels[i];
            if (nk_combo_begin_label(nk, cur_height, nk_vec2(nk_widget_width(nk), 200 * K))) {
                nk_layout_row_dynamic(nk, 25 * K, 1);
                for (int i = 0; i < 6; i++) {
                    if (nk_combo_item_label(nk, height_labels[i], NK_TEXT_LEFT))
                        s->export_height = heights[i];
                }
                nk_combo_end(nk);
            }

            /* Output path. */
            nk_layout_row_dynamic(nk, 25 * K, 1);
            nk_label(nk, s->export_path[0] ? s->export_path : "(no output file)", NK_TEXT_LEFT);
            nk_layout_row_dynamic(nk, 25 * K, 1);
            if (nk_button_label(nk, "Save as...")) {
                SDL_ShowSaveFileDialog(save_dialog_callback, s, s->window, NULL, 0, NULL);
            }

            /* Export button / progress. */
            enum export_state estate = s->exporter ? export_get_state(s->exporter) : EXPORT_IDLE;
            if (estate == EXPORT_RUNNING) {
                nk_layout_row_dynamic(nk, 25 * K, 1);
                nk_size prog = (nk_size)(export_get_progress(s->exporter) * 100);
                nk_progress(nk, &prog, 100, NK_FIXED);
                nk_layout_row_dynamic(nk, 25 * K, 1);
                if (nk_button_label(nk, "Cancel")) {
                    export_cancel(s->exporter);
                }
            } else {
                if (estate == EXPORT_DONE) {
                    nk_layout_row_dynamic(nk, 25 * K, 1);
                    nk_label(nk, "Export complete!", NK_TEXT_LEFT);
                } else if (estate == EXPORT_ERROR) {
                    nk_layout_row_dynamic(nk, 25 * K, 1);
                    const char *err = export_get_error(s->exporter);
                    nk_label(nk, err ? err : "Export failed", NK_TEXT_LEFT);
                } else if (estate == EXPORT_CANCELLED) {
                    nk_layout_row_dynamic(nk, 25 * K, 1);
                    nk_label(nk, "Export cancelled", NK_TEXT_LEFT);
                }
                nk_layout_row_dynamic(nk, 25 * K, 1);
                const int can_export = s->scene_loaded && s->export_path[0];
                if (can_export && nk_button_label(nk, "Export")) {
                    /*
                     * The export worker will pull a fresh scene snapshot from
                     * the rendering thread before encoding, so live edits are
                     * picked up. We just configure it here.
                     */
                    export_freep(&s->exporter);
                    s->exporter = export_create();
                    if (s->exporter) {
                        const struct export_params ep = make_export_params(s);
                        export_start(s->exporter, &ep);
                    }
                }
            }
            nk_tree_pop(nk);
        }
export_section_done:

        /* Hooks section. */
        if (s->hooks) {
            nk_layout_row_dynamic(nk, 10 * K, 1);
            nk_spacing(nk, 1);

            if (nk_tree_push(nk, NK_TREE_TAB, "Remote Sessions", NK_MAXIMIZED)) {
                nk_layout_row_dynamic(nk, 25 * K, 1);
                if (nk_button_label(nk, "Refresh Sessions"))
                    viewer_hooks_refresh(s);

                /* Session list with per-session enable toggle. */
                for (size_t i = 0; i < s->nb_hooks_sessions; i++) {
                    nk_layout_row_dynamic(nk, 25 * K, 1);
                    char label[256];
                    snprintf(label, sizeof(label), "%s (%s)",
                             s->hooks_sessions[i].id,
                             s->hooks_sessions[i].description);
                    nk_checkbox_label(nk, label, &s->hooks_session_enabled[i]);
                }

                if (s->nb_hooks_sessions == 0) {
                    nk_layout_row_dynamic(nk, 25 * K, 1);
                    nk_label(nk, "(click Refresh to scan)", NK_TEXT_LEFT);
                }

                nk_tree_pop(nk);
            }
        }

        render_scene_graph_section(s, nk, K);

        /*
         * Error panel: shown only when last_error is non-empty (the most
         * recent script load or scene build failed). Cleared automatically
         * on the next successful (re)load.
         */
        if (s->last_error[0]) {
            nk_layout_row_dynamic(nk, 10 * K, 1);
            nk_spacing(nk, 1);
            const struct nk_color red = nk_rgb(220, 80, 80);
            nk_layout_row_dynamic(nk, 25 * K, 1);
            nk_label_colored(nk, "Error:", NK_TEXT_LEFT, red);
            const float panel_h_avail = (float)s->win_height * 0.35f;
            nk_layout_row_dynamic(nk, panel_h_avail, 1);
            const nk_flags flags = (nk_flags)NK_EDIT_BOX | (nk_flags)NK_EDIT_READ_ONLY;
            nk_edit_string_zero_terminated(nk, flags, s->last_error, (int)sizeof(s->last_error),
                                           nk_filter_default);
        }
    }
    nk_end(nk);

    /*
     * Properties panel: shown on the right when a node is selected in the
     * Scene Graph tree.
     */
    const int show_properties = s->selected_node != NULL;
    const float properties_w = show_properties ? (float)s->win_width * 0.22f : 0.0f;
    const float properties_x = (float)s->win_width - properties_w - margin;

    /* Preview section. */
    const float playback_bar_h = 50.0f * K;
    const float preview_x = panel_width + margin;
    const float right_pad = show_properties ? properties_w + margin : 0.0f;
    const float preview_w = (float)s->win_width - panel_width - margin * 2 - right_pad;
    const float preview_h = (float)s->win_height - margin * 3 - playback_bar_h;
    const float playback_y = margin + preview_h + margin;

    if (nk_begin(nk, "Preview", nk_rect(preview_x, margin, preview_w, preview_h),
                 NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_NO_SCROLLBAR)) {
        struct nk_rect bounds = nk_window_get_content_region(nk);
        s->preview_x = bounds.x;
        s->preview_y = bounds.y;
        s->preview_w = bounds.w;
        s->preview_h = bounds.h;
    } else {
        /* Window collapsed/hidden — zero so the compositor skips the blit
         * rather than drawing at the now-stale bounds. */
        s->preview_x = s->preview_y = s->preview_w = s->preview_h = 0.0f;
    }
    nk_end(nk);

    /* Playback strip. */
    if (nk_begin(nk, "Playback", nk_rect(preview_x, playback_y, preview_w, playback_bar_h),
                 NK_WINDOW_BORDER | NK_WINDOW_NO_SCROLLBAR)) {
        const float btn_w  = 40.0f * K;
        const float time_w = 280.0f * K;
        nk_layout_row_template_begin(nk, 25 * K);
        nk_layout_row_template_push_static(nk, btn_w);
        nk_layout_row_template_push_static(nk, btn_w);
        nk_layout_row_template_push_static(nk, btn_w);
        nk_layout_row_template_push_dynamic(nk);
        nk_layout_row_template_push_static(nk, time_w);
        nk_layout_row_template_end(nk);

        const int can_step = s->scene_loaded && s->framerate[0] > 0 && s->framerate[1] > 0;
        const int can_play = s->scene_loaded;

        if (icon_button(nk, K, ICON_STEP_BACK, can_step)) {
            s->paused = 1;
            double t = frame_time - (double)s->framerate[1] / (double)s->framerate[0];
            if (t < 0.0)
                t = 0.0;
            viewer_set_frame_time(s, t);
        }
        if (icon_button(nk, K, s->paused ? ICON_PLAY : ICON_PAUSE, can_play)) {
            s->paused ^= 1;
            if (!s->paused)
                s->clock_off = (int64_t)SDL_GetTicks() - (int64_t)(frame_time * 1000.0);
        }
        if (icon_button(nk, K, ICON_STEP_FWD, can_step)) {
            s->paused = 1;
            double t = frame_time + (double)s->framerate[1] / (double)s->framerate[0];
            if (t > s->duration)
                t = s->duration;
            viewer_set_frame_time(s, t);
        }

        /* Slider takes the dynamic middle column. Always rendered so the
         * row template stays satisfied even without a scene. */
        if (s->scene_loaded && s->duration > 0.0) {
            float seek = (float)(frame_time / s->duration);
            float new_seek = seek;

            const struct nk_rect bar = nk_widget_bounds(nk);
            if (nk_input_has_mouse_click_down_in_rect(&nk->input, NK_BUTTON_LEFT, bar, nk_true)
                && bar.w > 0.0f) {
                const float ratio = (nk->input.mouse.pos.x - bar.x) / bar.w;
                new_seek = NK_CLAMP(0.0f, ratio, 1.0f);
            }

            nk_slider_float(nk, 0.0f, &new_seek, 1.0f, 0.001f);
            if (new_seek != seek) {
                const double t = new_seek * s->duration;
                viewer_set_frame_time(s, t);
                s->clock_off = (int64_t)SDL_GetTicks() - (int64_t)(t * 1000.0);
            }
        } else {
            nk_spacing(nk, 1);
        }

        char timebuf[96];
        format_time_label(timebuf, sizeof(timebuf), frame_time, s->duration,
                          s->framerate[0], s->framerate[1]);
        nk_label(nk, timebuf, NK_TEXT_RIGHT);
    }
    nk_end(nk);

    if (show_properties) {
        nk_window_show(nk, "Properties", NK_SHOWN);
        const float properties_h = (float)s->win_height - margin * 2;
        if (nk_begin(nk, "Properties", nk_rect(properties_x, margin, properties_w, properties_h),
                     NK_WINDOW_BORDER | NK_WINDOW_TITLE | NK_WINDOW_CLOSABLE)) {
            render_properties_panel_body(s, nk, K);
        }
        nk_end(nk);
        if (nk_window_is_hidden(nk, "Properties"))
            viewer_set_selected_node(s, NULL);
    }

    /* Toggle SDL text input based on which Nuklear widget is edited. */
    nk_sdl_update_text_input(s);
}
