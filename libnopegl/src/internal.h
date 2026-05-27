/*
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

#ifndef INTERNAL_H
#define INTERNAL_H

#include <stdbool.h>
#include <stdlib.h>

#include "config.h"

#if defined(HAVE_VAAPI)
#include "vaapi_ctx.h"
#endif

#if defined(TARGET_ANDROID)
#include "android_ctx.h"
#endif

#if HAVE_TEXT_LIBRARIES
#include <ft2build.h>
#include FT_OUTLINE_H
#endif

#include "aabb.h"
#include "hud.h"
#include "math_utils.h"
#include "node2d.h"
#include <ngpu/ngpu.h>
#include "slug.h"
#include "nopegl/nopegl.h"
#include "params.h"
#include "utils/darray.h"
#include "utils/hmap.h"
#include "utils/job_queue.h"
#include "utils/pthread_compat.h"
#include "utils/refcount.h"
#include "utils/utils.h"

struct node_class;

NGLI_DECLARE_DARRAY_WITH_NAME(ngli_mat4_darray, struct ngli_mat4);
NGLI_DECLARE_DARRAY_WITH_NAME(ngli_f32_darray, float);
NGLI_DECLARE_DARRAY_WITH_NAME(ngli_node_darray, struct ngl_node *);

struct api_impl {
    int (*configure)(struct ngl_ctx *s, const struct ngl_config *config);
    int (*resize)(struct ngl_ctx *s, uint32_t width, uint32_t height);
    int (*get_viewport)(struct ngl_ctx *s, int32_t *viewport);
    int (*set_capture_buffer)(struct ngl_ctx *s, void *capture_buffer);
    int (*set_scene)(struct ngl_ctx *s, struct ngl_scene *scene);
    int (*prepare_draw)(struct ngl_ctx *s, double t);
    int (*draw)(struct ngl_ctx *s, double t, struct ngpu_fence *wait_fence, struct ngpu_fence **signal_fence);
    void (*reset)(struct ngl_ctx *s, int action);

    /* Invoke a callback with the backend ready to issue GPU work */
    int (*dispatch)(struct ngl_ctx *s, int (*fn)(struct ngl_ctx *, void *), void *arg);

    /* OpenGL */
    int (*gl_wrap_framebuffer)(struct ngl_ctx *s, uint32_t framebuffer);
};

void ngli_free_text_builtin_atlas(void *user_arg, void *data);

struct text_builtin_atlas {
    struct slug *slug;
    struct slug_glyph_data slug_data[256];
};

struct ngl_ctx {
    int configured;
    const struct api_impl *api_impl;
    struct ngpu_ctx *gpu_ctx;
    struct ngpu_graphics_state default_graphics_state;
    struct ngpu_rendertarget_layout default_rendertarget_layout;
    struct ngl_scene *scene;
    struct ngl_config config;
    struct ngl_backend backend;
    struct ngpu_viewport viewport;
    struct ngpu_scissor scissor;
    struct ngpu_rendertarget *current_rendertarget;
    struct ngli_mat4 default_modelview_matrix;
    struct ngli_mat4 default_projection_matrix;
    struct ngli_mat4_darray modelview_matrix_stack;
    struct ngli_mat4_darray projection_matrix_stack;
    struct ngli_mat4_darray transform_2d_stack;
    struct ngli_f32_darray opacity_2d_stack;
    struct ngli_mat4 projection_2d_matrix;
    float canvas_2d_width;
    float canvas_2d_height;
#define NGLI_MAX_CLIPS_2D 8
    struct ngli_clip2d clips_2d[NGLI_MAX_CLIPS_2D];
    size_t nb_clips_2d;
    struct ngpu_staging_buffer **update_staging_buffers;
    struct ngpu_staging_buffer **draw_staging_buffers;
    struct ngpu_staging_buffer *current_staging_buffer;

    /*
     * Array of nodes that are candidate to either prefetch (active) or release
     * (non-active). Nodes are inserted from bottom (leaves) up to the top
     * (root).
     */
    struct ngli_node_darray activitycheck_nodes;

    /*
     * Array of nodes that have a bounding box and that are candidate to
     * spatial queries.
     */
    struct ngli_node_darray bounding_box_nodes;
    struct ngli_node_darray intersecting_nodes;

    struct hmap *text_builtin_atlasses; // struct text_builtin_atlas
#if HAVE_TEXT_LIBRARIES
    FT_Library ft_library;
#endif

#if defined(HAVE_VAAPI)
    struct vaapi_ctx vaapi_ctx;
#endif
#if defined(TARGET_ANDROID)
    struct android_ctx android_ctx;
#endif
    struct hud *hud;
    int64_t cpu_update_time;
    int64_t cpu_draw_time;
    int64_t gpu_draw_time;

    struct ngli_queue background_queue;

    /*
     * Array of frame slots tracking the borrow/release state of the ngl_frames.
     * Protected by frame_slots_lock since ngl_draw() and ngl_frame_release()
     * may run concurrently on different threads (producer/consumer split).
     */
    pthread_mutex_t frame_slots_lock;
    struct ngli_frame_slot {
        /*
         * Non-NULL while the frame is held by the ngl_draw() caller.
         */
        struct ngl_frame *frame;
        /*
         * Fence supplied by the consumer via ngl_frame_release(). The next
         * ngl_draw() picking this slot inserts it as a GPU wait fence before
         * rendering.
         */
        struct ngpu_fence *release_fence;
    } *frame_slots;
    uint32_t nb_frame_slots;
};

#define NGLI_ACTION_KEEP_SCENE  0
#define NGLI_ACTION_UNREF_SCENE 1

int ngli_ctx_configure(struct ngl_ctx *s, const struct ngl_config *config);
int ngli_ctx_resize(struct ngl_ctx *s, uint32_t width, uint32_t height);
int ngli_ctx_get_viewport(struct ngl_ctx *s, int32_t *viewport);
int ngli_ctx_set_capture_buffer(struct ngl_ctx *s, void *capture_buffer);
int ngli_ctx_set_scene(struct ngl_ctx *s, struct ngl_scene *scene);
int ngli_ctx_prepare_draw(struct ngl_ctx *s, double t);
int ngli_ctx_draw(struct ngl_ctx *s, double t, struct ngpu_fence *wait_fence, struct ngpu_fence **signal_fence);
void ngli_ctx_reset(struct ngl_ctx *s, int action);

struct livectl {
    union ngl_livectl_data val;
    char *id;
    union ngl_livectl_data min;
    union ngl_livectl_data max;
};

#define NGLI_NODE_NONE 0xffffffff

enum node_state {
    NGLI_NODE_STATE_INIT_FAILED   = -1,
    NGLI_NODE_STATE_UNINITIALIZED = 0, /* post uninit(), default */
    NGLI_NODE_STATE_INITIALIZED   = 1, /* post init() or release() */
    NGLI_NODE_STATE_READY         = 2, /* post prefetch() */
};

struct ngl_node {
    const struct node_class *cls;
    struct ngl_ctx *ctx;
    struct ngl_scene *scene;

    void *opts;

    enum node_state state;
    bool prepared;
    bool is_active;

    bool force_release_prefetch;

    double visit_time;
    double last_update_time;

    int draw_count;

    int refcount;
    int ctx_refcount;

    struct ngli_node_darray children;
    struct ngli_node_darray draw_children; // children with a draw callback
    struct ngli_node_darray parents;

    char *label;

    void *priv_data;
};

NGLI_DECLARE_DARRAY_WITH_NAME(ngli_str_darray, char *);
NGLI_DECLARE_DARRAY_WITH_NAME(ngli_ptr_darray, uint8_t *);

struct ngl_scene {
    struct ngli_rc rc;
    struct ngl_scene_params params;
    struct ngli_node_darray nodes; // set of all the nodes in the graph
    struct ngli_str_darray files; // files path strings (array of char *)
    struct ngli_ptr_darray files_par; // file based parameters pointers (array of uint8_t *)
};

enum node_category {
    NGLI_NODE_CATEGORY_NONE,
    NGLI_NODE_CATEGORY_VARIABLE,
    NGLI_NODE_CATEGORY_TEXTURE,
    NGLI_NODE_CATEGORY_BUFFER,
    NGLI_NODE_CATEGORY_BLOCK,
    NGLI_NODE_CATEGORY_IO,
    NGLI_NODE_CATEGORY_DRAW, /* node executes a graphics ngpu_pipeline */
    NGLI_NODE_CATEGORY_TRANSFORM,
};

#define NGLI_NODE2D_TYPES_LIST (const uint32_t[]){ \
    NGL_NODE_CANVAS2D,                             \
    NGL_NODE_DRAWRECT2D,                           \
    NGL_NODE_EFFECT2D,                             \
    NGL_NODE_GROUP2D,                              \
    NGL_NODE_OFFSCREENCANVAS2D,                    \
    NGL_NODE_TIMERANGEFILTER2D,                    \
    NGL_NODE_USERSELECT2D,                         \
    NGL_NODE_USERSWITCH2D,                         \
    NGLI_NODE_NONE}                                \

/*
 * Node is an exposed live control.
 *
 * A few important notes when setting this flag:
 *
 * - the private node context must contain a livectl struct, and
 *   node_class.livectl_offset must point to it (we can not have any static
 *   check for this because 0 is a valid offset)
 * - an option named "live_id" must be exposed in the parameters (and
 *   associated with `livectl.id`)
 * - the value parameter can have any arbitrary name but must be present before
 *   "live_id", point to `livectl.val`, and has to be the first parameter
 *   flagged with NGLI_PARAM_FLAG_ALLOW_LIVE_CHANGE
 */
#define NGLI_NODE_FLAG_LIVECTL (1 << 0)

/*
 * Node is shareable
 */
#define NGLI_NODE_FLAG_SHAREABLE (1 << 1)

/*
 * Node is a 2D node participating in the 2D scene graph.
 *
 * Important notes when setting this flag:
 *
 * - the first member of the private node context must be a ngli_node2d_info struct
 * - the node must not be shared across multiple parents (the 2D hierarchy
 *   must form a tree, not a DAG); this is enforced at attachment time
 */
#define NGLI_NODE_FLAG_2D  (1 << 2)

/*
 * Specifications of a node.
 *
 * Description of the callbacks attributes:
 *
 * - reentrant:
 *    + yes: callback will be called multiple time in diamond shaped tree
 *    + no: callback will be called only once in a diamond shaped tree
 * - execution order:
 *    + leaf/children first: callbacks are called in ascent order
 *    + root/parents first: callbacks are called in descent order
 *    + loose: each node decides (implies a manual dispatch)
 * - dispatch:
 *    + manual: the callback takes over / decides the dispatch to the children
 *    + managed: internals (nodes.c) are responsible for running the descent
 *               into children (meaning it controls ascent/descent order)
 *    + delegated: alias for "manual + managed", meaning managed by default
 *                 unless the callback is defined which take over the default
 *                 behavior
 */
struct node_class {
    uint32_t id;
    enum node_category category;
    const char *name;


    /************************
     * Init stage callbacks *
     ************************/

    /*
     * Initialize the node private context.
     *
     * reentrant: no (comparing state against NGLI_NODE_STATE_INITIALIZED)
     * execution-order: leaf first
     * dispatch: managed
     * when: called during set_scene() / internal node_set_ctx()
     */
    int (*init)(struct ngl_node *node);

    /*
     * Prepare the node rendering resources.
     *
     * reentrant: no
     * execution-order: leaf first
     * dispatch: managed
     * when: called during set_scene() / internal node_set_ctx() (after init)
     */
    int (*prepare)(struct ngl_node *node,
                   const struct ngpu_graphics_state *graphics_state,
                   const struct ngpu_rendertarget_layout *rendertarget_layout);

    /*
     * Override the render state passed to children during prepare.
     *
     * Useful for nodes that change the graphics state or rendertarget layout
     * of their subtree (GraphicConfig, RenderToTexture, Texture2D).
     */
    void (*get_child_render_state)(const struct ngl_node *node,
                                   const struct ngpu_graphics_state *graphics_state,
                                   const struct ngpu_rendertarget_layout *rendertarget_layout,
                                   struct ngpu_graphics_state *child_graphics_state,
                                   struct ngpu_rendertarget_layout *child_rendertarget_layout);


    /*******************************
     * Draw/update stage callbacks *
     *******************************/

    /*
     * Allow a node to stop the descent into its children by optionally
     * changing is_active and forwarding the call to the children.
     *
     * The callback MUST forward the call, even if the purpose is to disable
     * the branch.
     *
     * reentrant: yes (potentially with a different is_active flag)
     * execution-order: root first
     * dispatch: delegated
     * when: first step during an api draw call
     */
    int (*visit)(struct ngl_node *node, bool is_active, double t);

    /*
     * Pre-allocate resources or start background processing so that they are
     * ready at update time (typically nope.media). Contrary to allocations
     * done in the init, the prefetched resources lifetime is reduced to active
     * timeranges.
     *
     * The symmetrical callback for prefetch is the release callback.
     *
     * reentrant: no (comparing state against NGLI_NODE_STATE_READY)
     * execution-order: leaf first
     * dispatch: managed
     * when: follows the visit phase, as part of
     *       ngli_node_honor_release_prefetch() (comes after the release)
     */
    int (*prefetch)(struct ngl_node *node);

    /*
     * Reset node update time (and other potential state used in the update) to
     * force an update during the next api draw call.
     *
     * reentrant: yes
     * execution-order: leaf first
     * dispatch: managed
     * when: any time a parameter is live-changed
     */
    int (*invalidate)(struct ngl_node *node);

    /*
     * Update CPU/GPU resources according to the time.
     *
     * reentrant: no (based on node last_update_time)
     * execution-order: loose
     * dispatch: manual
     * when: straight after ngli_node_honor_release_prefetch()
     */
    int (*update)(struct ngl_node *node, double t);

    /*
     * Execute pre-render work (render-to-texture, compute dispatches, blur
     * passes) before the main render pass begins.
     *
     * reentrant: yes
     * execution-order: loose
     * dispatch: delegated (default: ngli_node_pre_draw_children)
     */
    void (*pre_draw)(struct ngl_node *node);

    /*
     * Apply transforms and execute graphics and compute pipelines.
     *
     * reentrant: yes (because the leaf of a diamond tree must be drawn for
     *            each path)
     * execution-order: loose
     * dispatch: manual
     * when: after scene has been update for a given time (which can be a no-op
     *       since it's non reentrant)
     */
    void (*draw)(struct ngl_node *node);

    /*
     * Must release resources (allocated during the prefetch phase) that will
     * not be used any time soon, or query a stop to potential background
     * processing (typically nope.media).
     *
     * The symmetrical callback for release is the prefetch callback.
     *
     * reentrant: no (comparing state against NGLI_NODE_STATE_READY)
     * execution-order: root first
     * dispatch: managed
     * when: follows the visit phase, as part of ngli_node_honor_release_prefetch()
     */
    void (*release)(struct ngl_node *node);


    /************************
     * Exit stage callbacks *
     ************************/

    /*
     * Must delete everything not released by the release callback. If
     * implemented, the release callback will always be called before uninit.
     *
     * reentrant: no (comparing state against NGLI_NODE_STATE_READY)
     * execution-order: root first
     * dispatch: managed
     * when: called during set_scene() / internal node_set_ctx()
     */
    void (*uninit)(struct ngl_node *node);

    /*
     * Must delete any extra resources still held by the node. This is typically
     * useful to delete user-provided data.
     *
     * reentrant: no
     * execution-order: loose
     * dispatch: managed
     * when: called when the node is about be freed
     */
    void (*free)(struct ngl_node *node);

    char *(*info_str)(const struct ngl_node *node);
    size_t opts_size;
    size_t priv_size;
    const struct node_param *params;
    const char *params_id;
    size_t livectl_offset;
    size_t node2d_offset;
    uint32_t flags;
    const char *file;
};

/* Internal scene API */
int ngli_scene_deserialize(struct ngl_scene *s, const char *str);
char *ngli_scene_serialize(const struct ngl_scene *s);
char *ngli_scene_dot(const struct ngl_scene *s);
void ngli_scene_update_filepath_ref(struct ngl_node *node, const struct node_param *par);

struct aabb ngli_node_compute_children_bounding_box(struct ngl_node *const *children, size_t nb_children);

int ngli_node_prepare(struct ngl_node *node,
                      const struct ngpu_graphics_state *graphics_state,
                      const struct ngpu_rendertarget_layout *rendertarget_layout);
int ngli_node_visit(struct ngl_node *node, bool is_active, double t);
int ngli_node_honor_release_prefetch(struct ngl_node *scene, double t);
int ngli_node_update(struct ngl_node *node, double t);
int ngli_node_update_children(struct ngl_node *node, double t);
void ngli_node_pre_draw(struct ngl_node *node);
void ngli_node_pre_draw_children(struct ngl_node *node);
int ngli_prepare_draw(struct ngl_ctx *s, double t);
void ngli_node_draw(struct ngl_node *node);
void ngli_node_draw_children(struct ngl_node *node);
int ngli_node_invalidate_branch(struct ngl_node *node);

int ngli_node_attach_ctx(struct ngl_node *node, struct ngl_ctx *ctx);
void ngli_node_detach_ctx(struct ngl_node *node, struct ngl_ctx *ctx);

int ngli_is_default_label(const char *class_name, const char *str);
const struct node_param *ngli_node_param_find(const struct ngl_node *node, const char *key,
                                              uint8_t **base_ptrp);

#endif
