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

#include <nopegpu/nopegpu.h>

#include "filterschain.h"
#include "libnopeutil/bstr.h"
#include "libnopeutil/darray.h"
#include "libnopeutil/hmap.h"
#include "libnopeutil/memory.h"
#include "log.h"

/* Shader helpers */
#include "helper_misc_utils_glsl.h"
#include "helper_noise_glsl.h"
#include "helper_oklab_glsl.h"
#include "helper_srgb_glsl.h"

struct filterschain {
    NPRIV_DARRAY(const struct filter *) filters;
    struct ngli_filter_resource_darray resources; // combined resources (struct ngli_filter_resource)
    struct hmap *unique_filters;
    struct bstr *str;
    const char *source_name;
    const char *source_code;
    uint32_t helpers;
};

struct filterschain *ngli_filterschain_create(void)
{
    struct filterschain *s = npriv_try_calloc(1, sizeof(*s));
    return s;
}

int ngli_filterschain_init(struct filterschain *s, const char *source_name, const char *source_code, uint32_t helpers)
{
    s->helpers = helpers;
    s->str = npriv_bstr_create();
    s->unique_filters = npriv_hmap_try_create(NPRIV_HMAP_TYPE_STR);
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
        int len = snprintf(combined_res.name, sizeof(combined_res.name), "%s%zu_%s",
                           filter->name, s->filters.count, res->name);
        if (len >= sizeof(combined_res.name)) {
            LOG(ERROR, "filter resource name \"%s%zu_%s\" is too long", filter->name, s->filters.count, res->name);
            return NGL_ERROR_LIMIT_EXCEEDED;
        }
        if (npriv_darray_try_push(&s->resources, combined_res) < 0)
            return NGL_ERROR_MEMORY;
    }

    if (npriv_darray_try_push(&s->filters, filter) < 0)
        return NGL_ERROR_MEMORY;

    s->helpers |= filter->helpers;

    if (npriv_hmap_get_str(s->unique_filters, filter->name))
        return 0;
    return npriv_hmap_try_set_str(s->unique_filters, filter->name, (void *)filter->code);
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

    for (size_t i = 0; i < NPRIV_ARRAY_NB(helpers_mask_code); i++)
        if (s->helpers & helpers_mask_code[i].mask)
            npriv_bstr_printf(b, "%s\n", *helpers_mask_code[i].code);

    npriv_bstr_printf(b, "%s\n", s->source_code);

    /*
     * Add filter codes; we're not using the filters darray because the same
     * filter could appear several times in a given filters chain, and we want
     * to include its code only once.
     */
    const struct hmap_entry *entry = NULL;
    while ((entry = npriv_hmap_next(s->unique_filters, entry)))
        npriv_bstr_printf(s->str, "%s\n", (const char *)entry->data);

    npriv_bstr_printf(b, "void main() {\n    vec4 color = %s();\n", s->source_name);

    for (size_t i = 0; i < s->filters.count; i++) {
        const struct filter *filter = s->filters.data[i];
        npriv_bstr_printf(b, "    color = filter_%s(color, uv", filter->name);

        for (size_t j = 0; j < filter->resources.count; j++)
            npriv_bstr_printf(b, ", %s%zu_%s", filter->name, i, filter->resources.data[j].name);

        npriv_bstr_print(b, ");\n");
    }

    npriv_bstr_print(b, "    ngl_out_color = color;\n");
    npriv_bstr_print(b, "}\n");
    return npriv_bstr_strdup(b);
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
    npriv_darray_reset(&s->filters);
    npriv_darray_reset(&s->resources);
    npriv_bstr_freep(&s->str);
    npriv_hmap_freep(&s->unique_filters);
    npriv_freep(sp);
}
