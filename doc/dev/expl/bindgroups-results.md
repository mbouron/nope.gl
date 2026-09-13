# Immutable bindgroup implementation

The baseline from sections 9 and 11 of [the design](bindgroups.md) is implemented:

- Bindgroups are created from a complete descriptor and retain the layout,
  textures and buffers. Public binding mutation and the separate bindgroup
  initializer are removed.
- Layouts recycle CPU object storage and Vulkan descriptor sets after command
  buffers release their references. Free storage retains no resource references.
- Vulkan writes all descriptors in one call at creation, using reserved backing
  arrays. Draws perform no descriptor writes.
- Texture metadata uses one array in a shared dynamic uniform block. Existing
  shader names remain available through aliases. Uploaded uniform blocks bind at
  base offset zero, with a block-sized range and an execution-specific offset.
- Consumers reuse their current bindgroup while bindings remain unchanged.
  Static backend binds are suppressed; dynamic offsets, pipeline changes and
  relevant OpenGL state changes cause bindings to be applied again.
- Nuklear retains its complete font/projection bindgroup. Viewer blits replace
  their bindgroup when the supplied texture changes.
- Pipeline discard releases the current bindgroup and resolved resources while
  preserving the pipeline and layout. Immutable sampler changes rebuild both.

This checkout starts at `6e76f3c58`, earlier than the source-registration baseline
mentioned in the design. Its existing resource maps are refreshed at execution,
with equality checks in the pipeline. Activity-release callbacks discard the
bindings; reactivation restores the vertex, buffer and texture inputs, including
text background and foreground resources.

The optional cache of complete binding sets remains deferred in the production
implementation. A subsequent [cache experiment](bindgroups-cache-results.md)
compares capacities of two, four and eight in temporary builds.

## Validation

- The existing integration suite passed all 1,485 tests across OpenGL, OpenGL ES
  and Vulkan.
- A new activity/discard/reactivation regression passed on all three backends.
  It covers text, geometry, sampled textures and Gaussian blur.
- All four `libngpu` tests and all fourteen `libnopegl` tests passed.
- New GPU tests cover copying and retaining bindings, invalid descriptors,
  allocation recycling, dynamic binding order and limits, merged metadata with
  `no_metadata` entries, static bind suppression, in-place uniform uploads,
  texture replacement within a submission, release immediately after recording
  a bind, and resource reclamation after submissions retire.
- The pipeline regression draws identical texture planes with different metadata
  twice in one frame, forces staging-buffer growth between draws, discards before
  submission, and reuses the pipeline in another frame with a fresh texture.

Both the libraries and the tools, including the Nuklear and viewer consumers,
build successfully. Their binding patterns are exercised by the GPU tests;
interactive viewer UI testing and Android immutable YCbCr sampler changes have
not been exercised on this Linux host.

## Local measurements

### Software-rendered comparison

Measurements used Linux software rendering: llvmpipe, LLVM 22.1.8, Mesa
26.2.2-arch1.1. They are evidence about this workload and host, not a hardware GPU
performance claim.

Three temporary builds were compared: the original mutable pool, that pool with
batched writes/merged metadata/dynamic offsets and the same consumer comparisons
and discard handling, and the immutable implementation. Builds used
`debugoptimized` without graphics validation. The scene contains eight Gaussian
blur nodes, four noise inputs, a decoded 160×90 H.264 video shared by four
blur inputs, and eight text nodes, rendered to 320×180. It executes 45 draws per
active frame after initialization.

Each run draws 100 active frames, 20 idle frames and 20 reactivated frames.
Frames 31–100 are measured, after 30 warm-up frames. The CPU interval uses
`CLOCK_THREAD_CPUTIME_ID` from entry to `ngpu_ctx_begin_draw()` through return
from `ngpu_ctx_end_draw()`, including command recording and submission but
excluding the preceding scene update/video decode phase. Instrumentation prints
cumulative counters after the timed interval. Three runs were taken per backend
and implementation.

| Backend | Implementation | Median CPU ms/frame | Range of run means | Descriptor update calls/frame | Descriptors written/frame |
| --- | --- | ---: | ---: | ---: | ---: |
| OpenGL | Original pool | 1.782 | 1.718–1.785 | 0 | 0 |
| OpenGL | Pool with common changes | 1.782 | 1.693–1.869 | 0 | 0 |
| OpenGL | Immutable | 1.737 | 1.713–1.922 | 0 | 0 |
| Vulkan | Original pool | 0.139 | 0.105–0.150 | 156 | 156 |
| Vulkan | Pool with common changes | 0.118 | 0.107–0.137 | 45 | 156 |
| Vulkan | Immutable | 0.099 | 0.095–0.144 | 45 | 156 |

Run ranges overlap, so these timings do not establish a reliable marginal
speedup from immutability. Batching demonstrably reduces the driver call count.
The complete staging-buffer bindings change between frames in this workload,
so the immutable baseline still creates replacement objects and writes 156
Vulkan descriptors per frame. No complete-binding cache hit rate was measured.

All three variants issue 45 backend bind operations per active frame in this
scene. The separate static-binding GPU regression verifies that three binds
`A, A, B` result in two backend binding calls; the dynamic version makes three.

The two mutable variants allocate 720 CPU bindgroup objects during warm-up,
compared with 90 for the immutable implementation, on both backends. None of the
variants allocates additional CPU bindgroup storage during frames 31–100.
Reactivation ends with 736 versus 92 allocated objects, respectively.

Tracked idle buffer and texture memory is identical for all three variants:

| Backend | Buffer bytes | Texture bytes | Reaches idle level |
| --- | ---: | ---: | --- |
| OpenGL | 292,160 | 9,540,608 | Second idle frame |
| Vulkan | 8,680,772 | 9,771,012 | Second idle frame |

These are `ngpu_memory_stats` resource sizes, excluding driver descriptor-pool
storage and CPU allocation pools. The higher Vulkan buffer total includes
texture staging/readback allocations. Recorded work can continue retaining
resources until its command-buffer references are retired.

### Hardware comparison

The command sandbox hides `/dev/dri`, which caused the software-rendering
fallback above. Running outside it selects the host's AMD Radeon RX 9070 XT:
radeonsi for OpenGL and RADV GFX1201 for Vulkan, with Mesa 26.2.2-arch1.1.

The same three instrumented builds and scene were rerun on this GPU, with six
runs per backend and implementation. Implementation order rotates, and backend
order alternates between runs. Each run renders 360 active frames at scene times
`frame / 120`, followed by 20 idle and 20 reactivated frames. Frames 61–360 are
measured, after 60 warm-up frames. The video remains 60 fps, so the measured
interval averages 44.5 draws per frame as video conversion work alternates.

CPU timing uses the same rendering-thread interval described above. A separate
wall-clock timer surrounds the Python `ctx.draw()` call, including scene update,
media handling and the instrumented draw. It does not measure GPU execution time
separately or force a GPU completion after every frame.

| Backend | Implementation | Median CPU ms/frame | Range of run means | Median `ctx.draw()` wall ms/frame |
| --- | --- | ---: | ---: | ---: |
| OpenGL | Original pool | 0.0989 | 0.0957–0.1044 | 0.7464 |
| OpenGL | Pool with common changes | 0.1121 | 0.1086–0.1218 | 0.7471 |
| OpenGL | Immutable | 0.1137 | 0.1117–0.1164 | 0.7476 |
| Vulkan | Original pool | 0.1570 | 0.1531–0.1631 | 0.7944 |
| Vulkan | Pool with common changes | 0.1433 | 0.1362–0.1458 | 0.7799 |
| Vulkan | Immutable | 0.1448 | 0.1408–0.1484 | 0.7856 |

Relative to the original pool, the immutable implementation reduces Vulkan
recording/submission CPU time by 7.8% (about 12 microseconds per frame), while
increasing OpenGL CPU time by 15.0% (about 15 microseconds per frame).
`ctx.draw()` wall time changes by +0.2% for OpenGL and -1.1% for Vulkan, with
overlapping run ranges; these results do not establish an overall frame-rate
improvement.

The pool with common changes already reduces Vulkan CPU time by 8.7% and
increases OpenGL CPU time by 13.4%. Immutability adds approximately 1% CPU time
relative to that intermediate variant on either backend, with overlapping
ranges. This workload therefore shows no additional timing benefit from
immutability itself.

On Vulkan, the original makes 154 descriptor-update calls per measured frame;
the other two variants make 44.5, a 71.1% reduction. All write 154 descriptors
and issue 44.5 binds per frame. Warm-up allocations remain 720 CPU bindgroup
objects for either mutable variant versus 90 for the immutable implementation,
an 87.5% reduction in object count. No additional CPU bindgroup storage is
allocated during the measured interval. Tracked idle resource memory remains
identical to the software comparison above; object counts do not measure total
driver or process memory.

The benchmark includes decoded video, but it does not measure Android
MediaCodec's fresh imported texture objects or immutable samplers. The follow-up
[cache experiment](bindgroups-cache-results.md) measures complete-set reuse,
CPU time and retained memory with imported, reused and freshly uploaded video
textures. Broader performance claims need additional workloads and hardware.
