# Bindgroup API design

Status: proposed.

Baseline: `image-rework-v3` at `8665dcc9f`, after the resource and pipeline
rework. The `ngli_pipeline` slots, source registrations and
`ngli_pipeline_discard_resources()` referred to below only exist there.
Measurements were taken on the OpenGL and Vulkan backends with the integration
tests.

## 1. The constraint everything follows from

A bindgroup must not be modified while a command buffer that refers to it is
still alive, from the moment it is recorded until the submission completes. On
Vulkan, with the descriptor flags used here, updating a descriptor set that a
command buffer in the recording or executable state has bound invalidates that
command buffer; `VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT` is not in use.
OpenGL has no descriptor sets but is not exempt: `gl_set_bindgroup()` records
only the bindgroup *pointer*, and `ngpu_bindgroup_gl_bind()` reads its binding
arrays at replay, so mutating a bindgroup between recording and submission
changes what an already-recorded command binds.

This is a correctness rule, not a cost. Every possible design is a way of
satisfying it, and the differences between them are only about where the
resulting bookkeeping lives. The existing pool satisfies it on both backends by
never selecting a bindgroup whose refcount shows the command buffer still holds
it.

It is worth being precise about what is *not* the problem:

- **Rotation is not the problem in the scenes measured.** Instrumenting
  `grow_bindgroup_array()` across `blur_gaussian`, `text_colors` and
  `drawrect2d_oval` shows every call is the initial allocation and the pool
  never regrows. That is a statement about those scenes, not a general
  property. Selection scans the pool as of `340a157cd`, and the scan is not
  redundant in general: growth *appends* slots, so circular index order stops
  tracking usage age, and a slot later in the ring can be free while the first
  candidate is still busy. Probing only the first candidate would grow the pool
  again in that case.
- **Rewriting a binding is not expensive in itself.** It is a few stores. On
  OpenGL there is no descriptor write at all; the `BindBufferRange` and
  `BindTexture` calls happen at bind time regardless.

What costs is how often descriptors are rewritten, and how. On `blur_gaussian`
the Vulkan backend issues 140 `vkUpdateDescriptorSets` calls for 40 bindgroup
rewrites: one driver call per dirty binding rather than one batched call per
bindgroup. Of those 140 writes, 110 are buffer writes and only 30 are texture
writes. The buffer writes exist because the staging offset of a uniform block
is baked into the descriptor and moves on every draw.

## 2. What immutability does and does not buy

Mutability, resource ownership and caching are independent choices, and it is
worth not conflating them:

- A mutable bindgroup *can* retain its bindings and release them on
  replacement. It did until recently. The problem was never that it retained,
  but that it kept retaining while idle, which is a reclamation policy question.
- An immutable bindgroup held by a cache pins its resources until evicted. The
  same policy question reappears, with a different trigger.
- Immutability does not remove duplicated binding state. The `ngli_pipeline`
  slots remain the canonical desired state and keep their own copy either way.

The one thing immutability does buy is worth the change on its own: it makes
the rule in §1 **unrepresentable**. There is no mutation, so there is no window
in which a recorded command buffer can observe a binding change, on either
backend. The pool, the refcount probing and the rotation exist only to police
that window, and all of it goes away.

What it costs is a cache and an eviction policy, which is new machinery that
did not exist before. §3 is where the real design risk lives, not §2.

## 3. Binding sets as values

```c
struct ngpu_bindgroup_desc {
    const struct ngpu_bindgroup_layout *layout;
    const struct ngpu_texture_binding *textures;
    size_t nb_textures;
    const struct ngpu_buffer_binding *buffers;
    size_t nb_buffers;
};

struct ngpu_bindgroup *ngpu_bindgroup_create(struct ngpu_ctx *s, const struct ngpu_bindgroup_desc *desc);
void ngpu_bindgroup_freep(struct ngpu_bindgroup **sp);
```

`ngpu_bindgroup_update_texture()` and `ngpu_bindgroup_update_buffer()` go away.
An immutable bindgroup retains what it binds, since it exists exactly as long
as its binding set is wanted. The command buffer keeps referencing the
bindgroup for the duration of a submission, as it does today.

This resembles the `ngpu_bindgroup_params.resources` that existed before the
rework and was removed in `1dda3286c`; the difference is that it becomes the
only way to populate a bindgroup rather than an optional shortcut, and the
backends would have to be restored to consuming it.

### 3.1 Ownership of the cache

Putting the cache on the layout is attractive because
`ngpu_bindgroup_layout_vk` already owns the descriptor pools and the free list
of recycled sets. It also creates a reference cycle:
`ngpu_bindgroup_vk_init()` does `s->layout = NGPU_RC_REF(params->layout)`, so
layout → cache → bindgroup → layout. That cycle must be broken deliberately,
and the design has to answer:

- **Who evicts, and when.** Eviction cannot be driven only by the pipelines
  using a layout, because a layout whose pipelines have all gone idle is
  exactly the case where its cache should shrink. A context-level sweep keyed
  on a frame counter, visiting every live layout, is the minimum.
- **What bounds the memory.** "A few frames" is not a bound. The cache needs a
  per-layout entry cap with LRU replacement, sized from the measurement asked
  for in §4.2, plus the sweep for layouts that stop being used.
- **What the cache holds onto.** Cached entries retain their textures and
  buffers. Releasing `s->bindgroup` in `ngli_pipeline_discard_resources()`
  would therefore no longer release those resources; the cache would. Discard's
  reclamation behaviour changes, and the sweep becomes the thing that actually
  frees GPU memory when a subtree goes idle. That is a real regression in
  promptness compared with the current discard, and it has to be measured
  before it is accepted.

The alternative is a per-pipeline cache, which keeps reclamation exactly where
it is today at the cost of no sharing between pipelines with the same layout.
Given the caveats in §4.2 about what the key actually contains, that may well
be the better trade.

## 4. Per-draw variation goes in dynamic offsets

If a uniform block's staging offset is part of the binding, the cache key
carries a value that has no reason to be stable. It is not true that every
lookup necessarily misses: `ngpu_staging_buffer_reset()` returns the offset to
zero on reuse, so a scene that allocates in the same order every frame produces
the same buffer, offset and range tuples again. What dynamic offsets buy is a
smaller set of distinct keys and, more importantly, independence from
allocation order — today a single conditional draw appearing or disappearing
shifts every subsequent offset in the frame and invalidates every key after it.

So this step is about making the key depend on what is bound rather than on how
the frame happened to be laid out. It is a precondition for reasoning about
cache size at all, not a guarantee of hits.

### 4.1 What the descriptor must contain

The descriptor must hold a **block-sized range**, normally at base offset zero,
not the whole allocation. The effective range on Vulkan is
`offset + dynamicOffset` through `offset + dynamicOffset + range`, and it must
lie inside the buffer, so a range covering the entire allocation overflows as
soon as the dynamic offset is non-zero. `validate_buffer()` already rejects
exactly this. The dynamic offset must also be a multiple of
`min_uniform_block_offset_alignment` (or the storage equivalent), which the
staging buffer's own alignment already satisfies.

Note that `gblur` and `distmap`, the two existing users, do not yet use
dynamic offsets this way: they bind at a per-frame base offset and use the
dynamic offset only for the delta between two draws inside one frame. Moving to
a fixed base is part of the work, not something already done.

### 4.2 Dynamic offsets do not produce one stable key

They remove the *offset* from the key, not the *buffer*. `ngl_ctx` holds
`nb_in_flight_frames` update staging buffers and `nb_in_flight_frames` draw
staging buffers, and selects between them per phase and per frame index
(`api.c:632` and `api.c:683`). With `nb_in_flight_frames` at 2 on both
backends, a pipeline therefore cycles through up to four distinct
`ngpu_buffer` pointers, and `ngpu_staging_buffer` replaces its buffer again
whenever it grows.

So the buffer half of the key settles into a **small recurring set**, not one
unchanging key. It also means `apply_buffer()` keeps seeing a changed binding
each frame, so `prepare_bindgroup()` does not short circuit and the pool does
not stop rotating. Step 3 eliminates offset-driven changes *within* a frame; it
does not make bindings stable *across* frames.

That recurrence applies only to the staging buffers, and only once growth has
settled. The key is the **whole** binding set, and a producer that hands out a
new object per frame defeats it entirely: the MediaCodec Vulkan mapper frees
and recreates `mc->texture` on every mapped frame, so a video pipeline's key
never recurs no matter what the offsets do. Dynamic offsets cannot fix that,
and a cache would miss once per frame for such a pipeline.

The entry cap in §3.1 therefore cannot be derived from the staging rotation
alone. Measure the distinct complete binding sets per pipeline, on a scene that
includes video, before choosing one.

### 4.3 The dynamic descriptor budget

`NGPU_MAX_UNIFORM_BUFFERS_DYNAMIC` is 8, matching Vulkan's guaranteed minimum
for `maxDescriptorSetUniformBuffersDynamic`. That budget is tight, because
`pgcraft` emits one `<name>_info` metadata block per texture. A pipeline with
four textures already has four info blocks plus its vertex, fragment and user
blocks, which is seven of the eight available.

So the info blocks must not each become dynamic. `pgcraft` should emit a single
metadata block holding an array indexed by texture index instead of one block
per texture. The budget is then a constant four dynamic uniform buffers —
vertex, fragment, user and metadata — regardless of texture count. Storage
buffers have a separate and smaller budget
(`NGPU_MAX_STORAGE_BUFFERS_DYNAMIC` is 4) and need the same check.

### 4.4 What this does not do

Dynamic offsets change how uploaded bytes are addressed, not whether they are
uploaded. Image metadata still carries a timestamp, coordinate and colour
transforms and a sampling mode that change per execution, and the test suite
covers two executions in one frame with identical texture planes but different
metadata. Merging the info blocks consolidates those uploads into one; avoiding
repeated uploads is a separate problem requiring change detection on the
metadata and reuse of a still-valid staging allocation.

## 5. The ngli side

The pool goes; everything else stays. Per execution:

```c
if (s->need_pipeline_recreation || !s->gpu_pipeline) {
    /* An immutable sampler change alters the layout itself, so the GPU
     * pipeline and the layout are still rebuilt here. A cached bindgroup
     * cannot be made compatible with a layout it was not created from, so
     * the cache is keyed by layout and the old entries simply age out. */
    ...
}

if (s->updated) {
    const struct ngpu_bindgroup_desc desc = {
        .layout      = s->bindgroup_layout,
        .textures    = <texture slot bindings>,
        .nb_textures = s->nb_textures,
        .buffers     = <buffer slot bindings>,
        .nb_buffers  = s->nb_buffers,
    };
    struct ngpu_bindgroup *bindgroup = ngpu_bindgroup_layout_get(s->bindgroup_layout, &desc);
    if (!bindgroup)
        return NGL_ERROR_MEMORY;
    ngpu_bindgroup_freep(&s->bindgroup);
    s->bindgroup = bindgroup;
    s->updated = 0;
}
ngpu_ctx_set_bindgroup(gpu_ctx, s->bindgroup, s->dynamic_offsets, s->nb_dynamic_offsets);
```

What disappears: `bindgroup_darray`, `select_next_available_bindgroup()`,
`grow_bindgroup_array()` and `NB_BINDGROUPS`. `updated` keeps its job of
telling the pipeline its resolved bindings changed.

The immutable sampler path must survive. On this baseline a sampler change sets
`need_pipeline_recreation`, and `prepare_bindgroup()` rebuilds the layout and
the GPU pipeline. Putting the sampler in the cache key does not help, because
the resulting bindgroup would still belong to the old layout.

`ngli_pipeline_discard_resources()` releases the resolved references, forgets
direct bindings and releases the cached bindgroup — with the caveat in §3.1
that the cache, not the discard, then decides when the resources are actually
freed. Note that `8665dcc9f` already preserves the pool and the GPU pipeline
across an ordinary discard, so avoiding that rebuild is no longer a motivation
for this proposal.

The slots stay the canonical desired state and keep their references, since the
cache lookup happens after resolution and before anything binds.

## 6. What does not change

- The command buffer owns execution lifetime and references the bindgroup and
  every bound buffer and texture for the duration of a submission.
- The layout's free list of recycled descriptor sets stays.
- OpenGL follows the API change but has no descriptor writes to save.

## 7. Order of work

1. **Batch `vkUpdateDescriptorSets`.** `ngpu_bindgroup_vk_update_descriptor_set()`
   issues one driver call per dirty binding; collect the writes into the
   `write_desc_sets` darray that already exists on the bindgroup and issue one
   call. Collecting the `VkWriteDescriptorSet` structures is not sufficient on
   its own: their `pImageInfo` and `pBufferInfo` currently point at loop-local
   variables. The batched version needs backing arrays for the
   `VkDescriptorImageInfo` and `VkDescriptorBufferInfo` values that stay valid
   *and do not move* until the final call, so they must be reserved up front
   from the known binding counts rather than grown while the writes are being
   collected. Independent of everything else.
2. **Merge the metadata blocks in `pgcraft`.** Self-contained, removes one
   binding per texture from every layout, and is the prerequisite for §4.3.
3. **Move per-draw uniform blocks to dynamic offsets**, with the fixed base
   offset and block-sized range of §4.1. About 60
   `ngli_pipeline_update_buffer()` call sites across the draw nodes, plus the
   block type changes in each node's crafter parameters.
4. **Measure.** See §8.
5. **Only then**, if the measurements justify it, make bindgroups immutable and
   add the cache.

Steps 1 to 3 are worth doing regardless of whether step 5 happens.

## 8. What must be measured before step 5

The 140-to-40 figure establishes fewer driver calls, not a speedup. Before
committing to the API change, on both backends and on a scene heavier than the
unit tests:

- CPU time in the submission path, not call counts.
- Descriptor writes per frame, before and after.
- Bindgroup allocations per frame, and cache hit rate over the distinct
  complete binding sets measured per §4.2, on a scene including video.
- Retained GPU memory while a subtree is idle, compared against the current
  discard, which releases promptly.

The criterion is the **marginal** benefit of step 5 over steps 1 to 3 already
done, weighed against the retained-memory cost of §3.1. A high hit rate with
few entries is evidence that a small cache is feasible, which argues *for* step
5, not against it; it is not by itself a reason to do it. The question is
whether removing the pool and the rotation buys measurable CPU time once the
descriptor writes have already been reduced.

There is also a third option that this document does not develop: keep the
mutable API and the current bindgroup lifetime, and replace the pool's
round-robin selection with a cache keyed on the binding set. If step 4 shows
the win is in avoiding redundant rewrites rather than in the API shape, this is
the cheaper way to get it, and it leaves the rule in §1 representable.

It does **not** avoid the ownership question of §3.1. What makes the pool safe
today is that `prepare_bindgroup()` rewrites every binding before a bindgroup
is reused, so the stale pointers a borrowed binding leaves behind are never
dereferenced. A cache hit skips that rewrite — that is the point of the hit —
and the protection goes with it. If buffer A is destroyed and buffer B is
allocated at the same address, a key of pointer, offset and size matches the
entry cached for A, whose descriptor still holds A's destroyed handle.
Confirming the bindgroup is not in flight does not detect this. Such a cache
needs either allocation generations that invalidate entries when a resource is
replaced or discarded, or retained references in the cached entry together with
an explicit policy for clearing the cache on discard, which is §3.1 again.

The lesson generalises: it is the **cache** that forces the retention question,
not the immutability. Any design that hands back an entry without rewriting it
has to be able to say why that entry is still valid.

One refinement applies to both designs. An exact, valid hit may reuse a
bindgroup that is still in flight: §1 forbids *modifying* one, not binding one
again with contents that have not changed. Only a miss, which has to write
bindings somewhere, needs an entry that is not in flight. Carrying the pool's
`refcount == 1` requirement over to hits would throw away exactly the reuse the
cache exists for.
