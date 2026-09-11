# Shared resource bindings and pipeline execution

Status: implemented, with refcounted resource holders and adapted to the
[immutable bindgroup API](bindgroups.md). Pipelines retain the two-entry
consumer-owned cache, shared image metadata, and absolute dynamic offsets. The original proposal was
based on `main` at `5c4a3e2b7` (`pgcraft: separate resource bindings from interface
descriptions`). Sections describing the previous implementation and the commit
sequence retain that historical context.

This document describes shared resource resolution and binding-change detection
across nope.gl Draw nodes, Compute nodes, and internal rendering passes.

## 1. Proposed result

Rename the nope.gl `pipeline_compat` helper to `pipeline`, with the
`ngli_pipeline_*` function prefix. Give it a table of resource sources and refresh
that table automatically before each draw or dispatch. A source identifies where
to obtain the current resource; registration does not require a GPU allocation.

Each pipeline compares the resolved bindings with its cached bindings. This
centralizes the work currently performed through `blocks_map`, `textures_map`,
`buffer_rev`, `image.rev`, and explicit vertex-buffer rebinding in individual
consumers.

The intended responsibilities are:

| Component | Responsibility |
| --- | --- |
| Resource owner | Create, initialize, upload, replace, and release its resources |
| Refcounted resource holder | Own the currently published allocation or image snapshot |
| `ngpu_pgcraft` | Describe and generate the shader interface and resolve binding indices |
| `ngli_pipeline` | Resolve sources, refresh bindings and image metadata, manage bindgroups, and execute |
| `ngpu_pipeline` | Represent the backend GPU pipeline |
| Draw/Compute node | Choose sources, prepare its own parameters, and request execution |

Public `ngl.Buffer*`, `ngl.Block`, and texture node interfaces remain suitable.
UBO, SSBO, vertex, and index usage describe how a consumer uses an allocation.
They do not require separate public resource classes.

## 2. Current implementation and its limitations

The previous commit made `pgcraft` descriptions independent of GPU resources.
Callers now supply resources explicitly through
[pipeline_compat](source:libnopegl/src/pipeline_compat.c). Its binding arrays are
allocated from the generated layouts and populated separately.

Several consumers still implement the same resource-tracking logic themselves:

| Current code | Behavior |
| --- | --- |
| [DrawTexture](source:libnopegl/src/node_drawtexture.c), DrawMask, DrawDisplace, and related nodes | Maintain texture maps, compare image revisions, and apply reframing separately |
| [pass.c](source:libnopegl/src/pass.c) | Refreshes images each execution and checks Block revisions |
| [DrawRect2D](source:libnopegl/src/node_drawrect2d.c) and [Effect2D](source:libnopegl/src/node_effect2d.c) | Maintain their own image and Block maps |
| [ColorStats](source:libnopegl/src/node_colorstats.c) | Replaces its stats buffer and increments `buffer_rev` |
| [Text](source:libnopegl/src/node_text.c) | Explicitly rebinds character buffers after allocation changes |
| [Geometry](source:libnopegl/src/geometry.h) and [Buffer](source:libnopegl/src/node_buffer.c) | Store GPU pointers obtained from another owner |

The consumer-side revision fields are not a complete dependency mechanism.
Vertex buffers have no equivalent common tracking, and copying a Block's buffer
pointer into a Buffer view and then Geometry loses the connection to the owner.
An allocation replacement cannot propagate through that chain automatically.

Image revisions also have a broader meaning than binding identity. For example,
`handle_media_frame()` increments the image revision for a newly mapped frame,
including frames uploaded into existing texture planes. A new timestamp or new
pixels need not change any descriptor binding.

Finally, the current `update_buffer()` and texture update helper mark the
bindgroup dirty even when the supplied binding equals the previous one. The
pipeline already holds enough information to do that comparison itself.

## 3. Naming and implementation boundary

Use the following names for the nope.gl helper:

| Current name | Proposed name |
| --- | --- |
| `pipeline_compat.c` / `pipeline_compat.h` | `pipeline.c` / `pipeline.h` |
| `struct pipeline_compat` | `struct pipeline` |
| `struct pipeline_compat_params` | `struct pipeline_params` |
| `ngli_pipeline_compat_*` | `ngli_pipeline_*` |
| Members named `pipeline_compat` | `pipeline` |

The `ngli_` and `ngpu_` prefixes distinguish the engine helper from the lower-level
GPU object. Inside `struct pipeline`, name the underlying `ngpu_pipeline` member
`gpu_pipeline` to make that distinction visible.

The rename should be a mechanical commit before behavior changes. Update all
includes, callers, forward declarations, and the source list in
`libnopegl/meson.build`. No compatibility aliases are needed for this private
engine interface.

Keep binding resolution in the nope.gl layer. `pgcraft` should not regain node
pointers, source pointers, allocation callbacks, or initial resource data.
The GPU library should continue receiving concrete bindings.

Declare source descriptions in `pipeline.h`, with typed source tables owned by
`pipeline.c`; extract implementation into a separate file only
if its size warrants that. This does not require a new scene traversal or a
general event/subscription framework.

## 4. Three kinds of change

The implementation must distinguish the following cases.

| Change | Example | Required action |
| --- | --- | --- |
| Contents | CPU upload or compute writes into the same buffer | Existing upload/synchronization path; bindings can remain identical |
| Binding | New buffer, changed range, different image plane or sampler | Refresh the affected resolved binding |
| Interface | Different Block field schema, attribute format/stride, or shader resource type | Revalidate and, where necessary, rebuild the shader/pipeline interface |

Image metadata is ordinary parameter data associated with execution. Its
timestamp, dimensions, color transforms, and coordinate transforms must stay
current independently of texture-binding comparisons.

An image layout change among sampling variants already generated by `pgcraft`
can be handled by selecting the appropriate texture slots and metadata. An
immutable sampler change can require GPU pipeline/layout recreation, as it
already does in `pipeline_compat`. Neither case justifies blindly rebuilding the
shader for every new media frame.

## 5. Buffer sources and views

### 5.1 Resource holders

A source is a heap-allocated, refcounted `struct buffer_resource`,
`struct texture_resource`, or `struct image_resource`, declared in
[resource.h](source:libnopegl/src/resource.h). Each opaque holder contains its
current buffer, raw texture, or image. Typed constructors, getters, setters, and
pipeline registrations let the compiler diagnose incompatible resource types.
An image holder allocates its image storage once, when the holder is created.

All three private representations begin with `struct ngli_rc`, enforced by
compile-time assertions. Allocation and reference counting share this prefix;
the reference count's destructor releases the appropriate publication. The
holders are structurally identical, so their reference helpers stay typed as
well: `ngli_buffer_resource_ref()` and `ngli_buffer_resource_freep()`, plus
their texture and image counterparts. A shared `void *` helper is deliberately
avoided, since it would let a holder of one kind be stored in a slot of another
without a diagnostic. No runtime type tag or union is needed.

The holder makes the intent explicit: consumers follow its current publication,
which may change independently of the holder's lifetime. This is an engine
abstraction; an `ngpu_buffer` or `ngpu_texture` still identifies a concrete GPU
allocation. A holder can be shared by several binding slots and consumers.

```c
struct buffer_resource *resource = ngli_buffer_resource_create();
if (!resource)
    return NGL_ERROR_MEMORY;

/* After the producer has initialized a replacement GPU buffer: */
ngli_buffer_resource_set(resource, buffer); /* Retains buffer. */
ngpu_buffer_freep(&buffer);                /* Drops the producer's temporary ref. */
```

Setters retain the new GPU references before releasing the old publication.
An empty buffer holder resolves to NULL. Consumers must never receive a
half-initialized allocation. Getters return borrowed values, valid until the
next publication; keep a GPU reference if a value must outlive that operation.

A Block and an independent Buffer each create a holder. A Block-backed Buffer
retains the Block's holder instead of copying its allocation:

```c
struct buffer_info {
    struct buffer_resource *resource;
    struct buffer_layout layout;
    /* CPU data, Block reference, usage, etc. */
};

struct block_info {
    struct buffer_resource *resource;
    /* Block description, CPU data, usage, etc. */
};

view->resource = ngli_resource_ref(block_info->resource);
```

A holder's address stays stable even if producer storage moves or disappears.
Every owner of a holder reference releases it with `ngli_resource_unrefp()`.
All publication, resolution, and holder-reference operations run on the render
thread; reference counting does not provide concurrent publication semantics.

### 5.2 Binding ranges

A buffer source registration takes an allocation source and a logical range:

```c
int ngli_pipeline_set_buffer_source(struct ngli_pipeline *s, int32_t index,
                                   const struct buffer_resource *resource,
                                   size_t offset, size_t size);
```

The pipeline retains the resource holder and stores the offset and size in its
buffer slot. It resolves `NGPU_BUFFER_WHOLE_SIZE` as the remaining allocation at
execution, so a variable length Block automatically follows a resized allocation.

Normalize a range only after verifying that the resource is ready:

1. Obtain the current buffer and its allocation size.
2. Require `offset <= allocation_size`.
3. Resolve a whole-size range as `allocation_size - offset`.
4. Require `size <= allocation_size - offset`, avoiding addition overflow.
5. Validate the binding's size, alignment, usage, and device limits.

Both the source and the direct update API require an explicit size or the
`NGPU_BUFFER_WHOLE_SIZE` sentinel; a zero size is rejected. The direct update
API briefly kept `size == 0` as shorthand for the whole allocation during the
migration, but that spelling ignored `offset` and has been removed now that
every caller passes an explicit range. Zero must never cause a size query
through a null buffer.

UBO and SSBO sources use the same registration API. The generated binding layout
determines the role and access requirements. For dynamic bindings, the source
range or direct update supplies an absolute offset. The pipeline normalizes the
descriptor offset to zero and supplies the absolute offset at execution, in the
layout's dynamic binding order. Validate the effective accessed range and
alignment on every execution. Offset-only changes do not invalidate bindgroups.

### 5.3 Geometry and attributes

Geometry retains resource holders for vertices, UVs, normals, and indices. It
creates holders for buffers it generates itself; its external-buffer setters
retain the supplied holder.

The complete lookup must be:

```text
vertex binding -> shared resource holder -> current GPU buffer
                         ^
                  Geometry/Buffer/Block
```

Geometry may copy stable attribute layout descriptions. It must not copy the
current GPU pointer. Apply the same rule to custom and instance attributes in
`pass.c`, and remove its cached `s->indices` allocation pointer.

The current GPU API accepts a vertex buffer without a base offset. Attribute
offsets and strides belong to the pipeline's vertex layout. Preserve that
arrangement: a Block field offset is represented there and must not also be
added as a runtime buffer offset.

Likewise, the current index setter accepts a buffer and index format without a
base offset. This proposal refreshes index-buffer identity through a source;
draw counts remain execution arguments. Changes to counts, formats, strides, or
field offsets require their existing validation and appropriate interface update.
The current restriction on Block-backed index buffers can remain.

Supporting a future pool that suballocates vertex/index data at movable base
offsets will require extending those backend binding APIs. Source indirection
alone does not provide that feature.

## 6. Image and raw texture sources

### 6.1 Image publication

An image combines texture planes, samplers, and metadata. Its holder retains a
refcounted `ngli_image`, which owns its plane textures. The producer can release
its own reference after publication:

```c
struct image_resource *input = ngli_image_resource_create();
if (!input)
    return NGL_ERROR_MEMORY;
ngli_pipeline_set_image_source(pipeline, image_index, input);

/* Publish after creating or selecting the input for this execution. */
ngli_image_resource_set(input, ngli_rtt_get_image(rtt, 0));
```

This same API handles persistent Texture images and replaceable RTT inputs.
There is no direct/indirect source distinction or borrowed CPU image address.
Replacing or freeing an RTT cannot invalidate an already published snapshot.

Publish again whenever planes or metadata change, including timestamp,
dimensions, or coordinate matrices. Texture, custom texture, RTT, offscreen
canvas, and blur producers do this at their mutation sites. Publication itself
only changes references. The current RTT and blur producers may also patch the
coordinates matrix of a retained image; the pipeline reads current metadata
independently for each execution and each consumer's reframing.

`ngli_image_resource_set(input, NULL)` publishes an empty image with layout NONE.
Use it when releasing an input or after a transient pass if its publication is
no longer needed. Required storage images fail validation when empty; sampled
images retain the existing transparent fallback behavior.

### 6.2 Reframing belongs to the image binding

Store optional reframing information alongside each image-source registration.
For the current Draw nodes, the concrete adapter can be a borrowed reframing
node, evaluated through `ngli_transform_chain_compute()` in the shared image
refresh path. Its lifetime follows the same graph dependency as the image.

Each consumer has its own reframing registration. Two Draw nodes sharing an
image can consequently use different transforms without modifying producer data.

Combine reframing with the image coordinates matrix before uploading the image
metadata block. Preserve the current matrix multiplication order. This removes
the sequence that first pushes ordinary metadata in `update_image()` and then
pushes a second block in `apply_reframing_matrix()` on a revision change.

### 6.3 Raw GPU textures

Internal code such as Text and HUD also binds raw textures without an image
metadata block. Retain a direct texture-binding setter and provide a source
registration for a TEXTURE holder, such as the current curve or band texture
in the text context. HUD uses the direct setter for its static private texture.

Raw textures and image-expanded planes feed the same concrete texture-binding
comparison. A raw source does not need to construct an artificial `struct image`
or allocate image metadata.

Imported textures require a sampler-lifetime audit. The `immutable_sampler`
pointer in a binding must remain valid through layout creation and execution.
Retain its owning texture or an explicit sampler reference according to the
backend contract. An unowned sampler address is insufficient as a cached identity.

## 7. Pipeline registration and execution APIs

The following signatures illustrate the target shape, with exact naming left to
the implementation:

```c
int ngli_pipeline_set_buffer_source(struct pipeline *s, int32_t index,
                                   const struct buffer_resource *resource,
                                   size_t offset, size_t size);
int ngli_pipeline_set_vertex_source(struct pipeline *s, int32_t index,
                                  const struct buffer_resource *resource);
int ngli_pipeline_set_index_source(struct pipeline *s,
                                 const struct buffer_resource *resource,
                                 enum ngpu_format format);
int ngli_pipeline_set_image_source(struct pipeline *s, int32_t image_index,
                                 const struct image_resource *resource);
int ngli_pipeline_set_texture_source(struct pipeline *s, int32_t index,
                                   const struct texture_resource *resource);

struct pipeline_execution {
    struct ngpu_staging_buffer *staging;
};

int ngli_pipeline_draw(struct pipeline *s,
                      const struct pipeline_execution *execution,
                      uint32_t nb_vertices, uint32_t nb_instances,
                      uint32_t first_vertex);
int ngli_pipeline_draw_indexed(struct pipeline *s,
                              const struct pipeline_execution *execution,
                              uint32_t nb_indices, uint32_t nb_instances);
int ngli_pipeline_dispatch(struct pipeline *s,
                          const struct pipeline_execution *execution,
                          uint32_t x, uint32_t y, uint32_t z);
```

Resolve shader names and stages once through `pgcraft` before registering slots.
An image index identifies a generated logical texture description, which can
expand into several sampler slots and one entry in the shared metadata array.
It is not a raw sampler index. This distinction must be visible in naming and bounds checks.

An optimized-out resource has index `-1`. Preserve the current not-found
convention; registration must not retain it, allocate metadata for it, or later
require an allocation. Reject invalid positive indices and conflicting bindings.

Each effective slot has one mode: a persistent source or a directly supplied
binding. Keep direct setters for caller-owned staging ranges and explicitly
selected transient resources. A direct setter targeting a source-controlled
slot should report a conflict until that source is explicitly removed. Image
registration reserves its generated sampler slots as well. The pipeline reserves
the shared metadata buffer at initialization; callers cannot bind that buffer
directly. Each image has a dense metadata index, with `no_metadata` entries
excluded, and all metadata is uploaded in one block per execution.

Common internal functions apply concrete bindings after resolution. Both source
refresh and direct setters use these functions, so they share equality, lifetime,
and dirty-state handling. Source refresh must not accidentally unregister itself
by calling a public direct setter.

Return errors from the execution helpers instead of silently swallowing refresh
or bindgroup failures. Node draw callbacks currently return `void`; those callers
can log the failure and skip their operation. Callers already returning an error,
including `ngli_pass_exec()`, should propagate it. This does not require changing
the public draw API or every node callback signature.

For example, the resource-related part of a DrawTexture preparation would become
the following. The names refer to the proposed Geometry members, and ordinary
error checks are omitted here; optimized-out indices still follow the convention
described above.

```c
/* Prepare: resolve slots and register sources once. */
const int32_t position_index =
    ngpu_pgcraft_get_vertex_buffer_index(crafter, "position");
const int32_t uvcoord_index =
    ngpu_pgcraft_get_vertex_buffer_index(crafter, "uvcoord");
ngli_pipeline_set_vertex_source(pipeline, position_index, geometry->vertices);
ngli_pipeline_set_vertex_source(pipeline, uvcoord_index, geometry->uvcoords);

/* DrawTexture's single logical image occupies image index 0. */
ngli_pipeline_set_image_source(pipeline, 0, texture_info->resource, texture_node);

/* Draw: after children and the node's own uniform data are prepared. */
const struct pipeline_execution execution = {
    .staging = ctx->current_staging_buffer,
};
ret = ngli_pipeline_draw(pipeline, &execution, nb_vertices, 1, 0);
```

The indexed variant registers the index source during preparation and passes the
index count to `ngli_pipeline_draw_indexed()`. Neither variant contains a resource
revision loop or a separate image-metadata/reframing update loop.

## 8. Execution sequence

Every draw and dispatch enters the same internal preparation function. It runs
after the caller has executed its resource-producing children or earlier passes
and prepared its own per-execution uniform data.

```text
producer update / earlier pass execution
                  |
caller writes its own uniform data into staging
                  |
ngli_pipeline_draw / draw_indexed / dispatch
                  |
resolve sources and validate current resources
                  |
apply image layout and consumer reframing
upload image metadata to this execution's staging buffer
                  |
compare and retain concrete bindings
                  |
prepare GPU pipeline and look up a complete immutable bindgroup
                  |
bind pipeline, resources, vertex/index inputs and dynamic offsets
                  |
issue draw or dispatch
```

Run resolution for each execution, including multiple executions within one
frame. A single per-frame refresh could miss an RTT replacement, a changed
intermediate input, or different metadata between two uses of a pipeline.

The execution context provides the current staging buffer. Do not save a
frame-specific staging pointer in the pipeline at initialization. The staging
buffer is required only when that execution needs generated image metadata;
pipelines without it can execute without a staging allocation.

Viewport, scissor, render-target selection, and render-pass boundaries stay with
their existing callers. Refreshing bindings does not schedule producer execution
or insert a new scene pass.

## 9. Comparing and applying concrete bindings

### 9.1 Comparison keys

Use explicit field comparisons rather than `memcmp()` on structures containing
padding:

| Binding | Comparison |
| --- | --- |
| Uniform/storage buffer | GPU buffer identity, normalized offset, normalized size |
| Raw texture or image plane | GPU texture identity and immutable sampler identity |
| Vertex buffer | GPU buffer identity under the current vertex layout |
| Index buffer | GPU buffer identity and index format |
| Dynamic offsets | Current values supplied for execution |

An image layout selects which concrete texture bindings to produce. Compare the
resulting slots, including cleared slots; no separate image revision is needed.
The layout also appears in the per-execution metadata.

For each image, construct all relevant generated texture slots from a known empty
state before filling the active planes. Switching from YUV to a single-plane
image must clear the former chroma slots. Sampler changes must be handled even
when texture pointers are equal.

Always refresh metadata when its generated block is used. A new timestamp,
coordinate matrix, color matrix, mapping matrix, or dimensions must be visible
with unchanged plane pointers. A same-frame second draw can have a different
consumer transform, so cache allocation offsets only with an explicit execution
lifetime; the initial implementation should simply push one block per use.

### 9.2 Bindgroup dirtiness

Changing a buffer/texture binding marks descriptor state dirty. Changing only
vertex/index inputs or dynamic offsets does not require rewriting descriptors.
An immutable sampler change additionally marks the GPU pipeline/layout for
recreation.

Keep the current immutable group when its concrete binding set is unchanged.
Otherwise, query the two-entry consumer-owned cache with the layout identity and
complete ordered buffer/texture bindings. A hit can reuse a group even while it
is referenced by recorded or submitted work. A miss creates a complete immutable
group before evicting the least recently used cache entry. Vulkan batches the
descriptor writes at creation; cache hits and offset-only changes write none.

The cache owns references to its groups and their resources. It is cleared on
pipeline/layout recreation and consumer discard. Command-buffer references keep
already recorded draws valid across eviction or discard. Source resolution still
runs before every execution, so a cached group cannot bypass validation of a
withdrawn or invalid publication.

### 9.3 Failure and first-use state

Registration allocates source tables and records dependencies without querying
GPU allocation size. First execution validates that required sources are ready.
An unallocated required buffer is an error; do not draw with a stale previous
allocation or a null pointer.

Resolve and validate required inputs before issuing GPU work. If an update,
pipeline recreation, or bindgroup acquisition fails, skip that execution and
keep the desired state dirty for retry. Mark a group current only after it has
been populated successfully. A partially updated available group is not eligible
for execution until the next complete preparation succeeds.

The existing preparation path clears its dirty flag before all operations have
succeeded. The refactor should move that state transition to successful
completion. Layout recreation must preserve owned resource references while the
old bindgroups are destroyed. A failed recreation must not make an incompatible
old pipeline eligible for execution.

For absent sampled images, make the current empty-image policy explicit in the
migration tests. Clear obsolete plane bindings and update metadata; do not retain
the last frame accidentally. `NGLI_IMAGE_LAYOUT_NONE` alone does not guarantee a
transparent result in the generated shader. Use the established fallback behavior
or an explicit fallback image, and validate its output on each backend. Required
storage images must be present and compatible before execution.

## 10. Identity, ownership, and lifetime

### 10.1 GPU identity contract

Pointer comparison is sufficient only under this contract:

1. A published GPU object keeps its allocation and binding-visible handles for
   its lifetime.
2. Replacing an allocation or imported image view publishes a different GPU
   object.
3. Cached concrete bindings hold references to their GPU objects.
4. A source and its contents are not concurrently destroyed while being resolved.

Retaining the previous object prevents its address from being recycled into an
apparently identical new object. A borrowed pointer left in a cache after freeing
the object would violate the comparison rule.

The current backend bindgroups already retain buffers and textures. The helper's
desired-state arrays and vertex/index caches need an explicit ownership rule too:
they can outlive a bindgroup or exist before the first group has been populated.
Make those concrete caches own references. Acquire the new reference before
releasing the old one, and release cached references on reset/destruction.

`ngpu_texture_ref()` and `ngpu_buffer_ref()` expose reference acquisition for
const-qualified binding inputs. The holder and pipeline use these operations;
GPU-library reference-count internals stay out of nope.gl.

GPU command recording and submission retain their existing in-flight ownership.
Replacing a source must never mutate a bindgroup still referenced by submitted
or recorded work. Immutable groups can be reused without a reference-count
availability check.

Audit imported-image paths before removing revisions. Existing media import code
commonly creates new texture wrappers, while software uploads can reuse planes.
If any path changes binding-visible handles inside a surviving wrapper, either
change it to publish a new wrapper or include an object generation in the common
comparison. A generation, if needed, belongs to that mutable object contract and
must be checked by the shared path for every consumer.

### 10.2 Source lifetime and teardown

Source registration retains the holder. Freeing or moving producer storage does
not leave a dangling source address. Replacing the allocation or image within
a holder requires no consumer registration change.

Clearing a publication and dropping a reference have different meanings:

```c
/* Buffer producer release: surviving consumers should observe unavailability. */
ngli_buffer_resource_set(owner->resource, NULL);

/* Teardown: relinquish this reference; other references remain valid. */
ngli_resource_unrefp(&owner->resource);
```

Dropping a reference alone preserves the last publication for surviving
consumers. Independent owners clear on release/teardown; Buffer views and
Geometry references to another producer only drop their own reference, so they
do not clear a shared Block. Destroying the final holder reference releases its
current GPU references and any image storage.

Holder references do not replace scene dependencies. Reframing nodes, Block
schemas, and shader interfaces still follow graph attachment and validation
rules. Replacing the logical producer through a graph edit updates or rebuilds
the affected registrations. Partial preparation failure must release every
acquired holder and GPU reference.

Reference ownership can delay GPU destruction: an inactive pipeline's cached
bindings and cached bindgroups can retain previous allocations until evicted or
discarded. Expose one common cache-discard path for consumer release and context
reset, and measure retention during timerange tests. Immediate reclamation of all
copies after an owner release would need an additional cache-eviction policy;
polling on execution alone does not promise it.

Discarding resolved state preserves registered holder references:
the next execution resolves them again. It invalidates directly supplied values,
which the caller must supply again before use. Do not silently draw with cleared
direct bindings. Full teardown also removes registrations. This distinction must
be explicit in the reset API and its callers.

## 11. Allocation, initial upload, and usage collection

This refactor separates source registration from allocation timing. It can land
on main while the existing node lifecycle remains in place.

During migration, an owner that still creates a wrapper in `init()` must keep it
private until initialization succeeds in `prepare()`. Publish it through the
resource source once it is ready. A subsequent change can create the wrapper and
its allocation together because consumers no longer need its address in advance.

The lifecycle requirements are:

| Point | Required information |
| --- | --- |
| Describe shader interface | Types, formats, layouts, and declared resource roles |
| Register a source | Stable owner/view and generated binding index |
| Allocate an owner | Allocation size and the complete required usage flags |
| First execution | Ready allocation, initial contents established, compatible bindings |

Usage collection is still required. Shader reflection only describes that
pipeline's interface; it does not automatically collect every use of a shared
buffer throughout the scene. A future `collect_usage()` or `init_resources()`
phase can establish the allocation contract before owners allocate.

Initial CPU uploads remain the owner's responsibility. An independent Buffer can
allocate and upload during preparation, ahead of first use. A Block can retain
its existing first-update upload ordering provided that update precedes execution.
Registering or refreshing a binding must never trigger another initial upload.

This matters for GPU state: a compute shader can modify a Block that a later Draw
uses as vertex data or an SSBO. Resolving the same source should preserve those
contents. The binding layer must not reconstruct the buffer from its original
CPU data. The existing recommendation to separate CPU-updated parameter Blocks
from persistent GPU state Blocks remains applicable.

If an edit adds a new required usage, the owner must validate the allocation's
capabilities. Keep a compatible allocation; otherwise allocate a replacement,
preserve required GPU contents, and publish it when ready. The shared binding
path then updates consumers automatically. Updating a usage bitmask alone cannot
upgrade an existing GPU allocation, and rebinding cannot replace synchronization.

A buffer pool can supply replacements later. Start with reuse of complete
allocations; suballocation requires logical range/base-offset support where the
GPU APIs lack it. Pool sizing, budget limits, prewarming, retirement, and trimming
remain allocation-policy work. Binding refresh should not choose a pool size or
allocate foreign resources as a side effect.

## 12. Consumer migration

| Area | Required change |
| --- | --- |
| `node_buffer.c/.h`, `node_block.c/.h` | Expose refcounted buffer holders; preserve Block-view indirection and existing CPU upload behavior |
| `geometry.c/.h`, `node_geometry.c` | Store resource sources and keep vertex/index layout validation |
| `pass.c/.h` | Register Blocks, images, geometry, custom/instance attributes, and indices; remove per-execution maps and revision loops |
| `node_drawcolor.c`, `node_drawnoise.c`, `node_drawgradient.c` | Register geometry sources; remove unused foreign-resource arrays and loops |
| `node_drawtexture.c`, `node_drawmask.c`, `node_drawdisplace.c` | Register image sources and per-consumer reframing; remove revision checks and parallel reframing arrays |
| `node_drawhistogram.c`, `node_drawwaveform.c` | Register ColorStats' stable buffer source and any image sources |
| `node_drawrect2d.c`, `node_effect2d.c` | Register custom Blocks/images and preserve paint/shader declaration ordering; publish replaceable pass inputs through image holders |
| `node_text.c` | Register stable character-buffer owners and atlas texture holders; remove allocation-time consumer rebinding |
| `node_colorstats.c` | Publish its buffer replacement; register the same source with its own compute pipelines and external consumers |
| `node_gblur.c`, `node_hblur.c`, `node_fgblur.c` | Register persistent resources and selected intermediate images; keep pass-specific parameters explicit |
| `node_drawpath.c`, `hud.c`, `distmap.c`, `hwconv.c` | Migrate helper calls, adopt sources where owners can replace allocations, preserve dynamic offsets and pass ordering |
| Texture, RTT, canvas, custom texture, and blur producers | Remove revision increments after all consumers use the shared mechanism |

All registrations are per pipeline instance. Sharing a resource does not share
the consumer's last-applied state, shader slots, or reframing transform.

Internal passes with a different input for each execution may update a stable
resource holder or use a direct setter. They still use the common comparison and
execution path. They should not reintroduce local revision counters.

Tools that use `libngpu` directly, including viewer rendering helpers, do not
acquire an engine-node dependency merely to share this abstraction. Keep their
explicit GPU resource bindings. The implementation boundary is the engine helper
and its consumers.

## 13. Suggested commit sequence

### Commit 1: Rename the helper

Perform the `pipeline_compat` to `pipeline` rename and update the build source
list. Build the engine and run representative graphics/compute checks. Keep this
commit mechanically reviewable.

### Commit 2: Centralize concrete binding comparison and ownership

Add buffer reference acquisition, owned cached bindings, field comparisons,
correct first-use state, and retry-safe dirty handling. Direct update callers
continue to work. Keep node revision checks temporarily while establishing the
shared behavior underneath them.

### Commit 3: Introduce sources and the common execution path

Add the typed registration tables, execution staging context, image slot
expansion, metadata/reframing refresh, and required-resource validation. Wire
automatic refresh into graphics and compute execution. Migrate one image consumer
and the ColorStats/Histogram producer-consumer path to exercise the complete
mechanism. Introduce the shared Block owner representation in this step and
adapt existing Block allocation/access sites so the intermediate commit builds
without maintaining two authoritative GPU pointers.

### Commit 4: Preserve buffer indirection through Geometry and pass

Migrate independent Buffer owners, Block-backed Buffer views, Geometry,
built-in/custom/instance attributes, and index buffers. Ensure no intermediate
view retains an allocation pointer from initialization. Exercise the same Block
through compute and vertex consumption.

### Commit 5: Migrate all engine consumers

Convert the remaining Draw nodes, Text, 2D rendering, blur passes, and internal
helpers. Remove per-node binding maps where registration has taken over, including
maps which have become empty or unused.

### Commit 6: Remove revisions and audit release paths

After the consumer conversion and imported-object audit, remove `buffer_rev`,
`image.rev`, producer `image_rev` counters, and obsolete tracking comments.
Exercise failed initialization, release/prefetch, resize, and scene teardown with
the new ownership rules. Complete broad backend validation.

Moving allocation to a new lifecycle phase or implementing a resource pool can
follow separately. These changes should not be prerequisites for the binding
refactor's acceptance.

## 14. Validation plan

Use the existing render and API test infrastructure for observable behavior.
Add targeted internal tests only for lifetime/failure cases that cannot be
triggered through current public nodes. A test-only producer can publish a new
allocation without adding a public buffer-replacement API.

| Scenario | Observable requirement |
| --- | --- |
| Two consumers share one replaceable Block | Both read the new allocation regardless of draw order or skipped frames |
| Block view used as a vertex input | Geometry follows the owner's replacement; no stale pointer remains in the view chain |
| Compute writes a buffer later used for vertices | The rendered result uses computed contents and no binding refresh restores CPU data |
| Buffer contents change without reallocation | New contents are visible without changing binding identity |
| Whole-size Block binding grows/shrinks | The resolved descriptor range follows the new allocation and passes device limits |
| Registration precedes allocation | Registration succeeds; first use succeeds once the owner has published a ready resource |
| Required source remains unallocated or becomes invalid | No draw/dispatch uses stale or null required resources; error handling is repeatable |
| Same image planes, changing timestamp/coordinates | Shader metadata changes even though texture bindings compare equal |
| Two Draw nodes share an image with different reframing | Each output uses its own transform; producer metadata stays unchanged |
| Image switches between single-plane and multi-plane layouts | Correct planes are selected and obsolete sampler slots are cleared |
| RTT object is replaced on resize | Published image snapshot retains planes and never dereferences the freed RTT |
| Sampler changes with stable texture identity | Layout/pipeline handling observes the sampler change |
| Text grows beyond its allocated character capacity | New vertex buffers and atlas textures are used without node-local rebind loops |
| Bindgroup cache hits and evicts while GPU work is in flight | Each submitted draw retains the correct complete resource set |
| Multiple executions in one frame select different inputs | Each execution sees its selected resources and fresh metadata |
| Initialization, replacement, or group preparation fails | No invalid GPU operation occurs; cleanup and retry preserve ownership and dirty state |
| Timerange release/prefetch and scene reset | Sources cannot dangle; cached resources are reclaimed according to the documented cache policy |

Existing coverage to use includes geometry/custom attributes, Block data,
compute animation/particles, ColorStats scopes, texture/media sampling, texture
reframing, RTT resizing, custom 2D resources, blur, text live changes, HUD, and
scene lifetime/reconfiguration tests. Check names and capability gates in
[tests/meson.build](source:tests/meson.build) when selecting runs.

Run OpenGL, OpenGLES, and Vulkan where supported. Record capability skips rather
than treating unsupported compute or import paths as tested. Media import and
immutable sampler cases need the relevant hardware/platform; software uploads
cannot establish those contracts.

Build `libngpu`, `libnopegl`, and `ngl-tools`, run the relevant native tests, then
the integration suite once the conversion is complete. Existing tests primarily
establish rendering correctness; use temporary counters or a focused debug
capture when validating that unchanged bindings do not trigger updates.

Measure CPU preparation time, descriptor updates, pipeline recreations, staging
bytes, and retained GPU allocations for static scenes, animated metadata, and
frequent resource replacement. Source polling is linear in registered bindings,
as the existing revision loops are. Allocate tables during preparation; steady
source resolution should not allocate heap memory. Staging-buffer growth and bindgroup-cache misses can still allocate and must be
measured separately.

## 15. Completion criteria and follow-up boundaries

The binding refactor is complete when all engine draw/dispatch entry points use
the shared refresh path, replaceable foreign buffers and images are resolved
through stable sources, and consumer-local revision tracking has been removed.
Image metadata must remain correct independently of binding identity, and source
registration must work without an allocated GPU buffer.

Before deleting the counters, close the concrete audit items: imported texture
and sampler identity, holder references and image snapshot ownership, empty-image
behavior on each backend, and cache retention during release. These are
verification requirements for the chosen design, not reasons to keep duplicate
tracking in every Draw node.

Allocation timing, scene-wide usage collection, memory-pool policy, CPU/GPU state
ownership, and shader-interface invalidation retain their own responsibilities.
This implementation supplies the missing connection from an owner's current
resource to every consuming pipeline, which allows those changes to be developed
without depending on GPU pointers captured during initialization.
