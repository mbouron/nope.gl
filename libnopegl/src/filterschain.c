/*
 * Copyright 2023 Matthieu Bouron <matthieu.bouron@gmail.com>
 * Copyright 2023 Nope Forge
 * Copyright 2021-2022 GoPro Inc.
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

#include "filterschain.h"
#include <ngpu/ngpu.h>
#include "utils/bstr.h"
#include "utils/darray.h"
#include "utils/hmap.h"
#include "utils/memory.h"

/* Shader helpers */
#include "helper_misc_utils_glsl.h"
#include "helper_noise_glsl.h"
#include "helper_oklab_glsl.h"
#include "helper_srgb_glsl.h"

struct filterschain {
    NGLI_DARRAY(const struct filter *) filters;
    struct ngli_filter_resource_darray resources; // combined resources (struct ngli_filter_resource)
    struct hmap *unique_filters;
    struct bstr *str;
    const char *source_name;
    const char *source_code;
    uint32_t helpers;
};

struct filterschain *ngli_filterschain_create(void)
{
    struct filterschain *s = ngli_calloc(1, sizeof(*s));
    return s;
}

int ngli_filterschain_init(struct filterschain *s, const char *source_name, const char *source_code, uint32_t helpers)
{
    s->helpers = helpers;
    s->str = ngli_bstr_create();
    s->unique_filters = ngli_hmap_create(NGLI_HMAP_TYPE_STR);
    if (!s->str || !s->unique_filters)
        return NGL_ERROR_MEMORY;
    s->source_name = source_name;
    s->source_code = source_code;
    return 0;
}

int ngli_filterschain_add_filter(struct filterschain *s, const struct filter *filter)
{
    for (size_t i = 0; i < filter->resources.count; i++) {
        const struct ngli_filter_resource *res = &filter->resources.data[i];
        struct ngli_filter_resource combined_res = *res;
        snprintf(combined_res.name, sizeof(combined_res.name), "%s%zu_%s",
                 filter->name, s->filters.count, res->name);
        if (ngli_darray_push(&s->resources, combined_res) < 0)
            return NGL_ERROR_MEMORY;
    }

    if (ngli_darray_push(&s->filters, filter) < 0)
        return NGL_ERROR_MEMORY;

    s->helpers |= filter->helpers;

    if (ngli_hmap_get_str(s->unique_filters, filter->name))
        return 0;
    return ngli_hmap_set_str(s->unique_filters, filter->name, (void *)filter->code);
}

static const struct {
    uint32_t mask;
    const char * const * code; // double dimension to avoid initializer not being a constant
} helpers_mask_code[] = {
    {NGLI_FILTER_HELPER_MISC_UTILS, &helper_misc_utils_glsl},
    {NGLI_FILTER_HELPER_NOISE, &helper_noise_glsl},
    {NGLI_FILTER_HELPER_OKLAB, &helper_oklab_glsl},
    {NGLI_FILTER_HELPER_SRGB, &helper_srgb_glsl},
};

char *ngli_filterschain_get_combination(struct filterschain *s)
{
    struct bstr *b = s->str;

    for (size_t i = 0; i < NGLI_ARRAY_NB(helpers_mask_code); i++)
        if (s->helpers & helpers_mask_code[i].mask)
            ngli_bstr_printf(b, "%s\n", *helpers_mask_code[i].code);

    ngli_bstr_printf(b, "%s\n", s->source_code);

    /*
     * Add filter codes; we're not using the filters darray because the same
     * filter could appear several times in a given filters chain, and we want
     * to include its code only once.
     */
    const struct hmap_entry *entry = NULL;
    while ((entry = ngli_hmap_next(s->unique_filters, entry)))
        ngli_bstr_printf(s->str, "%s\n", (const char *)entry->data);

    ngli_bstr_printf(b, "void main() {\n    vec4 color = %s();\n", s->source_name);

    for (size_t i = 0; i < s->filters.count; i++) {
        const struct filter *filter = s->filters.data[i];
        ngli_bstr_printf(b, "    color = filter_%s(color, uv", filter->name);

        for (size_t j = 0; j < filter->resources.count; j++)
            ngli_bstr_printf(b, ", %s%zu_%s", filter->name, i, filter->resources.data[j].name);

        ngli_bstr_print(b, ");\n");
    }

    ngli_bstr_print(b, "    ngl_out_color = color;\n");
    ngli_bstr_print(b, "}\n");
    return ngli_bstr_strdup(b);
}

const struct ngli_filter_resource_darray *ngli_filterschain_get_resources(struct filterschain *s)
{
    return &s->resources;
}

void ngli_filterschain_freep(struct filterschain **sp)
{
    struct filterschain *s = *sp;
    if (!s)
        return;
    ngli_darray_reset(&s->filters);
    ngli_darray_reset(&s->resources);
    ngli_bstr_freep(&s->str);
    ngli_hmap_freep(&s->unique_filters);
    ngli_freep(sp);
}
