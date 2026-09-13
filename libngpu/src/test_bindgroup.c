/*
 * Copyright 2026 Matthieu Bouron <matthieu@mojo.video>
 * Copyright 2018-2022 GoPro Inc.
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
#include <string.h>

#include "bindgroup.h"
#include "ctx.h"
#include "utils/utils.h"

#if defined(BACKEND_GL) || defined(BACKEND_GLES)
#include "opengl/ctx_gl.h"
#include "opengl/glcontext.h"
static void (NGPU_GL_APIENTRY *bind_buffer_range)(GLenum, GLuint, GLuint, GLintptr, GLsizeiptr);
static unsigned bind_calls;
static void NGPU_GL_APIENTRY count_bind_buffer_range(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size)
{
    bind_calls++;
    bind_buffer_range(target, index, buffer, offset, size);
}
#endif

#if defined(BACKEND_VK)
#include "vulkan/ctx_vk.h"
static PFN_vkUpdateDescriptorSets update_descriptor_sets;
static PFN_vkCmdBindDescriptorSets bind_descriptor_sets;
static unsigned write_calls, descriptor_writes, vk_bind_calls;
static VKAPI_ATTR void VKAPI_CALL count_update_descriptor_sets(VkDevice device, uint32_t count,
                                                              const VkWriteDescriptorSet *writes,
                                                              uint32_t nb_copies, const VkCopyDescriptorSet *copies)
{
    write_calls++;
    descriptor_writes += count;
    update_descriptor_sets(device, count, writes, nb_copies, copies);
}
static VKAPI_ATTR void VKAPI_CALL count_bind_descriptor_sets(VkCommandBuffer cmd, VkPipelineBindPoint point,
                                                            VkPipelineLayout layout, uint32_t first, uint32_t count,
                                                            const VkDescriptorSet *sets, uint32_t nb_offsets,
                                                            const uint32_t *offsets)
{
    vk_bind_calls++;
    bind_descriptor_sets(cmd, point, layout, first, count, sets, nb_offsets, offsets);
}
#endif

static struct ngpu_texture *create_texture(struct ngpu_ctx *ctx, const uint8_t *color)
{
    struct ngpu_texture *texture = ngpu_texture_create(ctx);
    ngpu_assert(texture);
    const struct ngpu_texture_params params = {
        .type = NGPU_TEXTURE_TYPE_2D,
        .format = NGPU_FORMAT_R8G8B8A8_UNORM,
        .width = 1, .height = 1, .depth = 1,
        .usage = NGPU_TEXTURE_USAGE_SAMPLED_BIT | NGPU_TEXTURE_USAGE_TRANSFER_DST_BIT,
    };
    ngpu_assert(ngpu_texture_init(texture, &params) == 0);
    ngpu_assert(ngpu_texture_upload(texture, color, 0) == 0);
    return texture;
}

static void test_dynamic_layouts(struct ngpu_ctx *ctx)
{
    /* Dynamic offsets follow binding numbers, including mixed buffer types. */
    struct ngpu_bindgroup_layout_entry entries[] = {
        {.type=NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC, .binding=3, .stage_flags=NGPU_PROGRAM_STAGE_FRAGMENT_BIT},
        {.type=NGPU_TYPE_STORAGE_BUFFER_DYNAMIC, .binding=1, .stage_flags=NGPU_PROGRAM_STAGE_FRAGMENT_BIT},
        {.type=NGPU_TYPE_UNIFORM_BUFFER,         .binding=2, .stage_flags=NGPU_PROGRAM_STAGE_FRAGMENT_BIT},
    };
    struct ngpu_bindgroup_layout *empty_layout = ngpu_bindgroup_layout_create(ctx);
    ngpu_assert(empty_layout && ngpu_bindgroup_layout_init(empty_layout, &(struct ngpu_bindgroup_layout_desc){0}) == 0);
    struct ngpu_bindgroup *empty = ngpu_bindgroup_create(ctx, &(struct ngpu_bindgroup_desc){.layout=empty_layout});
    ngpu_assert(empty);
    ngpu_bindgroup_layout_freep(&empty_layout);
    ngpu_bindgroup_freep(&empty); /* Last layout reference owns the recycled storage. */

    struct ngpu_bindgroup_layout_desc desc = {.buffers=entries, .nb_buffers=3};
    struct ngpu_bindgroup_layout *layout = ngpu_bindgroup_layout_create(ctx);
    ngpu_assert(layout && ngpu_bindgroup_layout_init(layout, &desc) == 0);
    ngpu_assert(ngpu_bindgroup_layout_get_dynamic_offset_index(layout, 0) == 1);
    ngpu_assert(ngpu_bindgroup_layout_get_dynamic_offset_index(layout, 1) == 0);
    ngpu_assert(ngpu_bindgroup_layout_get_dynamic_offset_index(layout, 2) == -1);
    ngpu_bindgroup_layout_freep(&layout);

    const enum ngpu_type types[] = {NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC, NGPU_TYPE_STORAGE_BUFFER_DYNAMIC};
    const size_t limits[] = {NGPU_MAX_UNIFORM_BUFFERS_DYNAMIC, NGPU_MAX_STORAGE_BUFFERS_DYNAMIC};
    struct ngpu_bindgroup_layout_entry excessive[NGPU_MAX_DYNAMIC_OFFSETS + 1] = {0};
    for (size_t i = 0; i < 2; i++) {
        for (size_t j = 0; j <= limits[i]; j++)
            excessive[j] = (struct ngpu_bindgroup_layout_entry){.type=types[i], .binding=(uint32_t)j};
        desc = (struct ngpu_bindgroup_layout_desc){.buffers=excessive, .nb_buffers=limits[i]+1};
        layout = ngpu_bindgroup_layout_create(ctx);
        ngpu_assert(layout && ngpu_bindgroup_layout_init(layout, &desc) == NGPU_ERROR_GRAPHICS_LIMIT_EXCEEDED);
        ngpu_bindgroup_layout_freep(&layout);
    }
}

static void test_metadata(struct ngpu_ctx *ctx)
{
    struct ngpu_pgcraft_texture textures[7] = {0};
    for (size_t i = 0; i < NGPU_ARRAY_NB(textures); i++) {
        snprintf(textures[i].name, sizeof(textures[i].name), "tex%zu", i);
        textures[i].type = NGPU_PGCRAFT_TEXTURE_TYPE_2D;
        textures[i].stage = NGPU_PROGRAM_STAGE_FRAG;
        textures[i].no_metadata = i == 1;
    }
    struct ngpu_block_desc block = {0};
    ngpu_block_desc_init(ctx, &block, NGPU_BLOCK_LAYOUT_STD140);
    ngpu_assert(ngpu_block_desc_add_field(&block, "value", NGPU_TYPE_VEC4, 0) >= 0);
    const struct ngpu_pgcraft_block blocks[] = {
        {.name="vert", .instance_name="v", .type=NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC, .stage=NGPU_PROGRAM_STAGE_VERT, .block=&block},
        {.name="frag", .instance_name="f", .type=NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC, .stage=NGPU_PROGRAM_STAGE_FRAG, .block=&block},
        {.name="user", .instance_name="u", .type=NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC, .stage=NGPU_PROGRAM_STAGE_FRAG, .block=&block},
    };
    const struct ngpu_pgcraft_params params = {
        .vert_base = "void main() { ngl_out_pos = v.value + tex0_coord_matrix * vec4(1.0); }",
        .frag_base = "void main() { ngl_out_color = f.value + u.value + vec4(tex6_ts + tex2_dimensions.x); }",
        .textures = textures, .nb_textures = NGPU_ARRAY_NB(textures),
        .blocks = blocks, .nb_blocks = NGPU_ARRAY_NB(blocks),
    };
    struct ngpu_pgcraft *crafter = ngpu_pgcraft_create(ctx);
    ngpu_assert(crafter && ngpu_pgcraft_craft(crafter, &params) == 0);
    struct ngpu_bindgroup_layout_desc desc = ngpu_pgcraft_get_bindgroup_layout_desc(crafter);
    ngpu_assert(desc.nb_buffers == 4);
    struct ngpu_pgcraft_texture_infos infos = ngpu_pgcraft_get_texture_infos(crafter);
    ngpu_assert(infos.nb_metadata == 6 && infos.block_index >= 0);
    ngpu_assert(infos.infos[0].metadata_index == 0 && infos.infos[1].metadata_index == -1);
    ngpu_assert(infos.infos[2].metadata_index == 1 && infos.infos[6].metadata_index == 5);
    ngpu_assert(desc.buffers[infos.block_index].type == NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC);
    ngpu_pgcraft_freep(&crafter);
    ngpu_block_desc_reset(&block);
}

static void retire_submissions(struct ngpu_ctx *ctx)
{
    /* Drain both update and draw submissions. Free allocation pools retain no GPU resources. */
    for (uint32_t i = 0; i <= ngpu_ctx_get_nb_in_flight_frames(ctx); i++) {
        ngpu_assert(ngpu_ctx_begin_update(ctx) == 0);
        ngpu_assert(ngpu_ctx_end_update(ctx, NULL) == 0);
        ngpu_assert(ngpu_ctx_begin_draw(ctx) == 0);
        ngpu_ctx_begin_render_pass(ctx, ngpu_ctx_get_default_rendertarget(ctx));
        ngpu_ctx_end_render_pass(ctx);
        ngpu_assert(ngpu_ctx_end_draw(ctx, 0, NULL, NULL) == 0);
        ngpu_ctx_advance_frame(ctx);
    }
}

static void test_cache(struct ngpu_ctx *ctx)
{
    const struct ngpu_memory_stats initial_memory = *ngpu_ctx_get_memory_stats(ctx);
    ngpu_assert(!ngpu_bindgroup_cache_create(ctx, 0));
    ngpu_assert(!ngpu_bindgroup_cache_create(ctx, SIZE_MAX));
    ngpu_assert(!ngpu_bindgroup_cache_create(NULL, 2));
    struct ngpu_bindgroup_cache *cache = ngpu_bindgroup_cache_create(ctx, 2);
    ngpu_assert(cache);

    struct ngpu_bindgroup_layout_entry buffer_entries[] = {
        {.type=NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC, .binding=0, .stage_flags=NGPU_PROGRAM_STAGE_FRAGMENT_BIT},
        {.type=NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC, .binding=1, .stage_flags=NGPU_PROGRAM_STAGE_FRAGMENT_BIT},
    };
    struct ngpu_bindgroup_layout_entry texture_entries[] = {
        {.type=NGPU_TYPE_SAMPLER_2D, .binding=2, .stage_flags=NGPU_PROGRAM_STAGE_FRAGMENT_BIT},
        {.type=NGPU_TYPE_SAMPLER_2D, .binding=3, .stage_flags=NGPU_PROGRAM_STAGE_FRAGMENT_BIT},
    };
    struct ngpu_bindgroup_layout_desc layout_desc = {
        .textures=texture_entries, .nb_textures=2, .buffers=buffer_entries, .nb_buffers=2,
    };
    struct ngpu_bindgroup_layout *layouts[2];
    for (size_t i = 0; i < 2; i++) {
        layouts[i] = ngpu_bindgroup_layout_create(ctx);
        ngpu_assert(layouts[i] && ngpu_bindgroup_layout_init(layouts[i], &layout_desc) == 0);
    }
    ngpu_assert(ngpu_ctx_begin_update(ctx) == 0);
    struct ngpu_texture *red = create_texture(ctx, (const uint8_t[]){255, 0, 0, 255});
    struct ngpu_texture *green = create_texture(ctx, (const uint8_t[]){0, 255, 0, 255});
    const size_t stride = NGPU_MAX(16U, ngpu_ctx_get_limits(ctx)->min_uniform_block_offset_alignment);
    struct ngpu_buffer *buffers[2];
    for (size_t i = 0; i < 2; i++) {
        buffers[i] = ngpu_buffer_create(ctx);
        ngpu_assert(buffers[i] && ngpu_buffer_init(buffers[i], 2 * stride + 32, NGPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT) == 0);
    }
    struct ngpu_texture_binding textures[] = {{.texture=red}, {.texture=green}};
    struct ngpu_buffer_binding bindings[] = {{.buffer=buffers[0], .size=16}, {.buffer=buffers[1], .size=16}};
    const struct ngpu_bindgroup_desc desc = {
        .layout=layouts[0], .textures=textures, .nb_textures=2, .buffers=bindings, .nb_buffers=2,
    };
    struct ngpu_bindgroup *a = ngpu_bindgroup_cache_get(cache, &desc);
    ngpu_assert(a);

    for (size_t i = 0; i < 7; i++) {
        struct ngpu_texture_binding varied_textures[2];
        struct ngpu_buffer_binding varied_buffers[2];
        memcpy(varied_textures, textures, sizeof(textures));
        memcpy(varied_buffers, bindings, sizeof(bindings));
        struct ngpu_bindgroup_desc varied = desc;
        varied.textures = varied_textures;
        varied.buffers = varied_buffers;
        switch (i) {
        case 0: varied_buffers[0].buffer = buffers[1]; break;
        case 1: varied_buffers[0].offset = stride; break;
        case 2: varied_buffers[0].size = 32; break;
        case 3: varied_buffers[0].buffer = buffers[1]; varied_buffers[1].buffer = buffers[0]; break;
        case 4: varied_textures[0].texture = green; break;
        case 5: varied_textures[0].texture = green; varied_textures[1].texture = red; break;
        case 6: varied.layout = layouts[1]; break;
        }
        struct ngpu_bindgroup *different = ngpu_bindgroup_cache_get(cache, &varied);
        ngpu_assert(different && different != a);
        ngpu_bindgroup_freep(&different);
        struct ngpu_bindgroup *hit = ngpu_bindgroup_cache_get(cache, &desc);
        ngpu_assert(hit == a); /* Touching A keeps it while the other entry is evicted. */
        ngpu_bindgroup_freep(&hit);
    }

    struct ngpu_bindgroup_desc invalid = desc;
    invalid.nb_buffers = 1;
    ngpu_assert(!ngpu_bindgroup_cache_get(cache, &invalid));
    invalid.nb_buffers = 2;
    invalid.buffers = NULL;
    ngpu_assert(!ngpu_bindgroup_cache_get(cache, &invalid));
    textures[0].immutable_sampler = layouts[0];
    ngpu_assert(!ngpu_bindgroup_cache_get(cache, &desc));
    textures[0].immutable_sampler = NULL;
    ngpu_assert(!ngpu_bindgroup_cache_get(cache, NULL));
    ngpu_assert(!ngpu_bindgroup_cache_get(NULL, &desc));
    struct ngpu_bindgroup *hit = ngpu_bindgroup_cache_get(cache, &desc);
    ngpu_assert(hit == a); /* Invalid lookups must leave valid entries intact. */
    ngpu_bindgroup_freep(&hit);

    /* Eviction and clear must preserve previously returned references. */
    bindings[0].size = 32;
    struct ngpu_bindgroup *b = ngpu_bindgroup_cache_get(cache, &desc);
    bindings[0].size = 48;
    struct ngpu_bindgroup *c = ngpu_bindgroup_cache_get(cache, &desc);
    ngpu_assert(b && c && a->buffers[0].size == 16);
    bindings[0].size = 16;
    struct ngpu_bindgroup *a_miss = ngpu_bindgroup_cache_get(cache, &desc);
    ngpu_assert(a_miss && a_miss != a);
    ngpu_bindgroup_cache_clear(cache);
    ngpu_bindgroup_cache_clear(cache);
    ngpu_bindgroup_cache_freep(&cache);
    ngpu_bindgroup_cache_freep(&cache);
    ngpu_bindgroup_cache_clear(NULL);
    for (size_t i = 0; i < 2; i++) {
        ngpu_buffer_freep(&buffers[i]);
        ngpu_bindgroup_layout_freep(&layouts[i]);
    }
    ngpu_texture_freep(&red);
    ngpu_texture_freep(&green);
    ngpu_assert(ngpu_buffer_get_size(a->buffers[0].buffer) == 2 * stride + 32);
    ngpu_assert(ngpu_texture_get_params(b->textures[0].texture)->width == 1);
    ngpu_bindgroup_freep(&a);
    ngpu_bindgroup_freep(&b);
    ngpu_bindgroup_freep(&c);
    ngpu_bindgroup_freep(&a_miss);
    ngpu_assert(ngpu_ctx_end_update(ctx, NULL) == 0);
    retire_submissions(ctx);
    const struct ngpu_memory_stats *memory = ngpu_ctx_get_memory_stats(ctx);
    ngpu_assert(memory->buffer_bytes == initial_memory.buffer_bytes);
    ngpu_assert(memory->texture_bytes == initial_memory.texture_bytes);
}

static void test_bindgroups(struct ngpu_ctx *ctx, uint8_t *capture, bool dynamic, bool cached)
{
    struct ngpu_bindgroup_cache *cache = cached ? ngpu_bindgroup_cache_create(ctx, 2) : NULL;
    ngpu_assert(!cached || cache);
    const struct ngpu_memory_stats initial_memory = *ngpu_ctx_get_memory_stats(ctx);
    ngpu_assert(ngpu_ctx_begin_update(ctx) == 0);
    struct ngpu_texture *red = create_texture(ctx, (const uint8_t[]){255, 0, 0, 255});
    struct ngpu_texture *green = create_texture(ctx, (const uint8_t[]){0, 255, 0, 255});
    const uint32_t stride = (uint32_t)NGPU_MAX(16U, ngpu_ctx_get_limits(ctx)->min_uniform_block_offset_alignment);
    struct ngpu_buffer *buffer = ngpu_buffer_create(ctx);
    ngpu_assert(buffer);
    ngpu_assert(ngpu_buffer_init(buffer, stride + 16,
                                NGPU_BUFFER_USAGE_UNIFORM_BUFFER_BIT | NGPU_BUFFER_USAGE_TRANSFER_DST_BIT) == 0);
    const float white[] = {1, 1, 1, 1};
    ngpu_assert(ngpu_buffer_upload(buffer, white, 0, sizeof(white)) == 0);
    ngpu_assert(ngpu_buffer_upload(buffer, white, stride, sizeof(white)) == 0);

    struct ngpu_block_desc block = {0};
    ngpu_block_desc_init(ctx, &block, NGPU_BLOCK_LAYOUT_STD140);
    ngpu_assert(ngpu_block_desc_add_field(&block, "tint", NGPU_TYPE_VEC4, 0) >= 0);
    const struct ngpu_pgcraft_block blocks[] = {{
        .name = "color", .instance_name = "",
        .type = dynamic ? NGPU_TYPE_UNIFORM_BUFFER_DYNAMIC : NGPU_TYPE_UNIFORM_BUFFER,
        .stage = NGPU_PROGRAM_STAGE_FRAG, .block = &block,
    }};
    const struct ngpu_pgcraft_texture textures[] = {{
        .name = "tex", .type = NGPU_PGCRAFT_TEXTURE_TYPE_2D,
        .stage = NGPU_PROGRAM_STAGE_FRAG, .no_metadata = true,
    }};
    const struct ngpu_pgcraft_params params = {
        .vert_base = "void main() { float x = -1.0 + float((ngl_vertex_index & 1) << 2);"
                     "float y = -1.0 + float((ngl_vertex_index & 2) << 1); ngl_out_pos = vec4(x,y,0.0,1.0); }",
        .frag_base = "void main() { ngl_out_color = texture(tex, vec2(0.5)) * tint; }",
        .blocks = blocks, .nb_blocks = 1, .textures = textures, .nb_textures = 1,
    };
    struct ngpu_pgcraft *crafter = ngpu_pgcraft_create(ctx);
    ngpu_assert(crafter && ngpu_pgcraft_craft(crafter, &params) == 0);
    struct ngpu_bindgroup_layout_desc layout_desc = ngpu_pgcraft_get_bindgroup_layout_desc(crafter);
    struct ngpu_bindgroup_layout *layout = ngpu_bindgroup_layout_create(ctx);
    ngpu_assert(layout && ngpu_bindgroup_layout_init(layout, &layout_desc) == 0);
    struct ngpu_pipeline *pipeline = ngpu_pipeline_create(ctx);
    const struct ngpu_pipeline_params pipeline_params = {
        .type = NGPU_PIPELINE_TYPE_GRAPHICS,
        .program = ngpu_pgcraft_get_program(crafter),
        .layout.bindgroup_layout = layout,
        .graphics = {
            .topology = NGPU_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .state.color_write_mask = 15,
            .rt_layout = *ngpu_ctx_get_default_rendertarget_layout(ctx),
        },
    };
    ngpu_assert(pipeline && ngpu_pipeline_init(pipeline, &pipeline_params) == 0);
    struct ngpu_texture_binding texture_binding = {.texture = red};
    struct ngpu_buffer_binding buffer_binding = {.buffer = buffer, .size = sizeof(white)};
    struct ngpu_bindgroup_desc desc = {
        .layout = layout, .textures = &texture_binding, .nb_textures = 1,
        .buffers = &buffer_binding, .nb_buffers = 1,
    };

    /* Invalid complete descriptors must fail without consuming any references. */
    desc.nb_buffers = 0;
    ngpu_assert(!ngpu_bindgroup_create(ctx, &desc));
    desc.nb_buffers = 1;
    buffer_binding.size = ngpu_buffer_get_size(buffer) + 1;
    ngpu_assert(!ngpu_bindgroup_create(ctx, &desc));
    buffer_binding.size = sizeof(white);

    texture_binding.immutable_sampler = layout;
    ngpu_assert(!ngpu_bindgroup_create(ctx, &desc));
    texture_binding.immutable_sampler = NULL;

    /* Retired CPU storage is reused, with all previous resource references released. */
    const size_t red_refs = red->rc.count;
    const size_t buffer_refs = buffer->rc.count;
    struct ngpu_bindgroup *a = ngpu_bindgroup_create(ctx, &desc);
    ngpu_assert(a);
    struct ngpu_bindgroup *storage = a;
    ngpu_bindgroup_freep(&a);
    ngpu_assert(red->rc.count == red_refs && buffer->rc.count == buffer_refs);
    a = ngpu_bindgroup_create(ctx, &desc);
    ngpu_assert(a == storage);
    if (cached) {
        ngpu_bindgroup_freep(&a);
        a = ngpu_bindgroup_cache_get(cache, &desc);
        ngpu_assert(a);
    }
    texture_binding.texture = green;
    struct ngpu_bindgroup *b = cached ? ngpu_bindgroup_cache_get(cache, &desc) : ngpu_bindgroup_create(ctx, &desc);
    ngpu_assert(b && b != a);
    ngpu_assert(a->textures[0].texture == red && b->textures[0].texture == green);
    texture_binding.texture = NULL;
    buffer_binding.size = 0; /* Creation copied the arrays. */
    ngpu_texture_freep(&red);
    ngpu_texture_freep(&green);
    ngpu_assert(ngpu_ctx_end_update(ctx, NULL) == 0);

    for (unsigned frame = 0; frame < 2; frame++) {
        /* Like Nuklear's projection upload: content changes, bindings do not. */
        if (frame) {
            ngpu_assert(ngpu_ctx_begin_update(ctx) == 0);
            ngpu_assert(ngpu_buffer_upload(buffer, (const float[]){0.5f, 0.5f, 0.5f, 1.f}, 0, 16) == 0);
            ngpu_assert(ngpu_ctx_end_update(ctx, NULL) == 0);
        }
        ngpu_assert(ngpu_ctx_begin_draw(ctx) == 0);
        ngpu_ctx_begin_render_pass(ctx, ngpu_ctx_get_default_rendertarget(ctx));
        ngpu_ctx_set_scissor(ctx, &(struct ngpu_scissor){.width=2, .height=1});
        ngpu_ctx_set_pipeline(ctx, pipeline);
        ngpu_ctx_set_viewport(ctx, &(struct ngpu_viewport){.width=1, .height=1});
#if defined(BACKEND_GL) || defined(BACKEND_GLES)
        bind_calls = 0;
#endif
#if defined(BACKEND_VK)
        vk_bind_calls = 0;
        const unsigned previous_writes = write_calls;
#endif
        uint32_t offset = 0;
        ngpu_ctx_set_bindgroup(ctx, a, &offset, dynamic);
        ngpu_ctx_draw(ctx, 3, 1, 0);
        if (cached) {
            const struct ngpu_bindgroup_desc hit_desc = {
                .layout=a->layout, .textures=a->textures, .nb_textures=1, .buffers=a->buffers, .nb_buffers=1,
            };
            struct ngpu_bindgroup *hit = ngpu_bindgroup_cache_get(cache, &hit_desc);
            ngpu_assert(hit == a); /* Exact matches can be reused while referenced by recorded work. */
            ngpu_bindgroup_freep(&hit);
        }
        ngpu_ctx_set_bindgroup(ctx, a, &offset, dynamic);
        ngpu_ctx_draw(ctx, 3, 1, 0);
        ngpu_ctx_set_viewport(ctx, &(struct ngpu_viewport){.x=1, .width=1, .height=1});
        offset = stride;
        ngpu_ctx_set_bindgroup(ctx, b, &offset, dynamic);
        if (frame) {
            ngpu_assert(b->rc.count > 1);
            ngpu_bindgroup_freep(&b);
        }
        ngpu_ctx_draw(ctx, 3, 1, 0);
        ngpu_ctx_end_render_pass(ctx);
        if (frame) {
            /* Recorded commands keep the objects and their resources alive. */
            ngpu_bindgroup_cache_freep(&cache);
            ngpu_assert(a->rc.count > 1);
            ngpu_bindgroup_freep(&a);
            ngpu_bindgroup_freep(&b);
            ngpu_buffer_freep(&buffer);
            ngpu_pipeline_freep(&pipeline);
            ngpu_bindgroup_layout_freep(&layout);
        }
        ngpu_assert(ngpu_ctx_end_draw(ctx, 0, NULL, NULL) == 0);
        ngpu_ctx_wait_idle(ctx);
        const uint8_t expected[] = {frame ? 128 : 255, 0, 0, 255,
                                   0, frame && !dynamic ? 128 : 255, 0, 255};
        if (memcmp(capture, expected, sizeof(expected))) {
            fprintf(stderr, "capture mismatch (cached=%d, dynamic=%d, frame=%u):", cached, dynamic, frame);
            for (size_t i = 0; i < sizeof(expected); i++)
                fprintf(stderr, " %u/%u", (unsigned)capture[i], (unsigned)expected[i]);
            fprintf(stderr, "\n");
        }
        ngpu_assert(!memcmp(capture, expected, sizeof(expected)));
#if defined(BACKEND_GL) || defined(BACKEND_GLES)
        if (ngpu_ctx_get_backend_type(ctx) != NGPU_BACKEND_VULKAN)
            ngpu_assert(bind_calls == (dynamic ? 3 : 2));
#endif
#if defined(BACKEND_VK)
        if (ngpu_ctx_get_backend_type(ctx) == NGPU_BACKEND_VULKAN) {
            ngpu_assert(vk_bind_calls == (dynamic ? 3 : 2));
            ngpu_assert(write_calls == previous_writes);
        }
#endif
        ngpu_ctx_advance_frame(ctx);
    }
    ngpu_pgcraft_freep(&crafter);
    ngpu_block_desc_reset(&block);
    retire_submissions(ctx);
    const struct ngpu_memory_stats *memory = ngpu_ctx_get_memory_stats(ctx);
    ngpu_assert(memory->buffer_count == initial_memory.buffer_count);
    ngpu_assert(memory->buffer_bytes == initial_memory.buffer_bytes);
    ngpu_assert(memory->texture_count == initial_memory.texture_count);
    ngpu_assert(memory->texture_bytes == initial_memory.texture_bytes);

}

int main(int argc, char **argv)
{
    enum ngpu_backend_type backend = NGPU_BACKEND_OPENGL;
    if (argc > 1) {
        if (!strcmp(argv[1], "vulkan"))
            backend = NGPU_BACKEND_VULKAN;
        else if (!strcmp(argv[1], "opengles"))
            backend = NGPU_BACKEND_OPENGLES;
    }
    uint8_t capture[8] = {0};
    const struct ngpu_ctx_params params = {
#if defined(TARGET_DARWIN)
        .platform=NGPU_PLATFORM_MACOS,
#elif defined(TARGET_IPHONE)
        .platform=NGPU_PLATFORM_IOS,
#elif defined(TARGET_ANDROID)
        .platform=NGPU_PLATFORM_ANDROID,
#elif defined(TARGET_WINDOWS)
        .platform=NGPU_PLATFORM_WINDOWS,
#endif
        .backend=backend, .offscreen=1, .width=2, .height=1, .capture_buffer=capture, .debug=1,
    };
    struct ngpu_ctx *ctx = ngpu_ctx_create(&params);
    if (!ctx || ngpu_ctx_init(ctx) < 0) {
        ngpu_ctx_freep(&ctx);
        return 77;
    }
#if defined(BACKEND_GL) || defined(BACKEND_GLES)
    if (backend != NGPU_BACKEND_VULKAN) {
        struct glcontext *gl = ((struct ngpu_ctx_gl *)ctx)->glcontext;
        bind_buffer_range = gl->funcs.BindBufferRange;
        gl->funcs.BindBufferRange = count_bind_buffer_range;
    }
#endif
#if defined(BACKEND_VK)
    if (backend == NGPU_BACKEND_VULKAN) {
        struct vkcontext *vk = ((struct ngpu_ctx_vk *)ctx)->vkcontext;
        update_descriptor_sets = vk->funcs.UpdateDescriptorSets;
        bind_descriptor_sets = vk->funcs.CmdBindDescriptorSets;
        vk->funcs.UpdateDescriptorSets = count_update_descriptor_sets;
        vk->funcs.CmdBindDescriptorSets = count_bind_descriptor_sets;
    }
#endif
    retire_submissions(ctx); /* Warm the default targets and readback buffers. */
    test_dynamic_layouts(ctx);
    test_metadata(ctx);
    test_bindgroups(ctx, capture, false, false);
    test_bindgroups(ctx, capture, true, false);
#if defined(BACKEND_VK)
    if (backend == NGPU_BACKEND_VULKAN) {
        ngpu_assert(write_calls == 6 && descriptor_writes == 12);
    }
#endif
    test_cache(ctx);
    test_bindgroups(ctx, capture, false, true);
    test_bindgroups(ctx, capture, true, true);
    ngpu_ctx_freep(&ctx);
    return 0;
}
