# Bindgroup cache experiment

A two-entry cache per pipeline captured all observed recurring binding sets in
this workload. It substantially reduced Vulkan descriptor writes, with modest
CPU savings and no demonstrated overall frame-time improvement. Capacities of
four and eight added no hits and retained more resources when video textures
were replaced. This is evidence for a small optional cache, not a universal
capacity or performance guarantee.

The measured prototype is preserved in `/tmp/bindgroups-benchmark/cache`.
The production utility is implemented in `libngpu/src/bindgroup_cache.c`, with
two-entry instances owned by `ngli_pipeline`. It keeps the prototype's complete
key, linear LRU lookup and explicit clearing, removes benchmark instrumentation
and environment switches, and validates capacity allocation sizes. Nuklear and
the viewer continue using direct immutable bindgroup creation and reuse.

## Method

The GPU was the AMD Radeon RX 9070 XT, using radeonsi for OpenGL and RADV for
Vulkan, Mesa 26.2.2-arch1.1. Commands ran outside the device-restricted sandbox.
All timing comparisons use the same `debugoptimized` experimental build, with
capacity zero selecting the immutable baseline. Graphics validation was disabled
for timing. No frame output is captured during timed runs.

The prototype uses a consumer-owned, bounded LRU with linear lookup. Keys compare
layout identity, every ordered texture and immutable sampler identity, and every
buffer identity, base offset and range. Dynamic offsets are excluded. Hits
return a reference to an existing immutable bindgroup even if it is in flight.
Misses create one and evict the least recently used entry. Cached groups retain
their resources. Pipeline discard and rebuild clear the cache; destruction
releases its allocation. The current-group fast path remains in place.

The scene is the same 320×180 mix of eight Gaussian blurs, four noise inputs, a
160×90 60-fps H.264 video shared by four blur inputs, and eight text nodes used
in the [baseline measurements](bindgroups-results.md). Each run draws 360 active
frames at scene times `frame / 120`, then 20 idle and 20 reactivated frames.
Frames 61–360 are measured. Capacity order rotates; backend and workload order
alternate between repetitions.

CPU time covers the rendering thread from `ngpu_ctx_begin_draw()` through
`ngpu_ctx_end_draw()`, excluding the preceding scene update and decoding phase.
The wall timer surrounds Python `ctx.draw()`, including update, media handling,
and instrumentation output. It neither separately measures GPU execution time
nor forces GPU completion after each frame. Tables show medians of per-run
means; ranges show the minimum and maximum run mean.

Three video paths were checked:

- **Imported:** normal hardware decoding. Debug logs confirmed VAAPI DMA-BUF
  imports through EGL images on OpenGL and Vulkan images on Vulkan. Both
  mappers create fresh plane texture objects for each mapped video frame.
- **Reused uploads:** CPU video decoding (`hwaccel='disabled'`), while rendering
  remains on the GPU. The normal uploader reuses its plane textures.
- **Fresh uploads:** the same CPU decoding path, with an experimental hook
  replacing plane textures on every mapped video frame. This makes allocation
  retention measurable; it does not simulate Android's sampler or decoder
  lifetime rules.

There are 16 runs per capacity/backend for imported video and eight for each
controlled upload case. The imported totals combine two eight-run sweeps: a
software-upload replacement switch in the second sweep was inactive because
VAAPI bypasses that uploader. Controlled upload runs explicitly disabled
hardware decoding, and debug logs confirmed the default uploader.

## Timing

Normal hardware-decoded video:

| Backend | Capacity | CPU ms/frame | Range of run means | `ctx.draw()` wall ms/frame |
| --- | ---: | ---: | ---: | ---: |
| opengl | 0 | 0.1164 | 0.1120–0.1388 | 0.7737 |
| opengl | 2 | 0.1135 | 0.1083–0.1254 | 0.7785 |
| opengl | 4 | 0.1136 | 0.1086–0.1266 | 0.7755 |
| opengl | 8 | 0.1139 | 0.1066–0.1199 | 0.7781 |
| vulkan | 0 | 0.1478 | 0.1383–0.1704 | 0.8078 |
| vulkan | 2 | 0.1407 | 0.1329–0.1572 | 0.8135 |
| vulkan | 4 | 0.1415 | 0.1332–0.1600 | 0.8250 |
| vulkan | 8 | 0.1430 | 0.1337–0.1648 | 0.8193 |

Two entries versus the immutable baseline:

- opengl: CPU -2.5%; `ctx.draw()` wall time +0.6%.
- vulkan: CPU -4.8%; `ctx.draw()` wall time +0.7%.

Run ranges overlap. The results show a small CPU benefit in the medians, not a
reliable end-to-end speedup. Larger capacities do not consistently improve
timing.

Controlled video uploads, all still rendered on the Radeon GPU:

| Video textures | Backend | Capacity | CPU ms/frame | Range of run means | `ctx.draw()` wall ms/frame |
| --- | --- | ---: | ---: | ---: | ---: |
| reused | opengl | 0 | 0.0906 | 0.0851–0.0936 | 0.7680 |
| reused | opengl | 2 | 0.0883 | 0.0827–0.0920 | 0.7714 |
| reused | opengl | 4 | 0.0865 | 0.0815–0.0881 | 0.7637 |
| reused | opengl | 8 | 0.0859 | 0.0824–0.0905 | 0.7693 |
| reused | vulkan | 0 | 0.1458 | 0.1419–0.1672 | 0.8030 |
| reused | vulkan | 2 | 0.1345 | 0.1288–0.1423 | 0.7993 |
| reused | vulkan | 4 | 0.1327 | 0.1293–0.1432 | 0.8025 |
| reused | vulkan | 8 | 0.1382 | 0.1238–0.1566 | 0.8045 |
| fresh | opengl | 0 | 0.0888 | 0.0855–0.0961 | 0.7696 |
| fresh | opengl | 2 | 0.0860 | 0.0835–0.0914 | 0.7676 |
| fresh | opengl | 4 | 0.0869 | 0.0817–0.0929 | 0.7586 |
| fresh | opengl | 8 | 0.0862 | 0.0841–0.0924 | 0.7651 |
| fresh | vulkan | 0 | 0.1535 | 0.1420–0.1660 | 1.1995 |
| fresh | vulkan | 2 | 0.1456 | 0.1383–0.1558 | 1.1989 |
| fresh | vulkan | 4 | 0.1477 | 0.1377–0.1525 | 1.2116 |
| fresh | vulkan | 8 | 0.1440 | 0.1365–0.1516 | 1.1967 |

## Reuse and driver work

Hit rate counts actual cache lookups, after the current-group fast path. All
three nonzero capacities produced identical steady-state hit rates and driver
work on every run:

| Video textures | Hit rate, capacities 2/4/8 | Vulkan update calls/frame, baseline → cache | Vulkan descriptors written/frame, baseline → cache |
| --- | ---: | ---: | ---: |
| Imported | 98.876% | 44.5 → 0.5 | 154 → 2 |
| Reused uploads | 100% | 44 → 0 | 152 → 0 |
| Fresh uploads | 98.876% | 44.5 → 0.5 | 154 → 2 |

The imported and fresh-upload cases hit 44 times per measured frame and miss
0.5 times per frame, corresponding to the video frame mapping rate. A larger
cache cannot make newly created texture identities recur. Reused uploads have
44 cache hits per frame and no misses after warm-up; the conversion pipeline's
unchanged current group also avoids lookup on this path.

All variants issue 44.5 backend binds per frame. A cache avoids creation and
descriptor writing, but does not suppress binds needed for dynamic offsets or
pipeline changes. OpenGL has no descriptor-update calls to eliminate.

## Retained memory

With reused uploads, all capacities have the same measured active buffer and
texture memory as the baseline. Fresh uploaded video textures expose the cost
of retaining older complete binding sets:

| Capacity | Extra active GL texture bytes | Extra active Vulkan texture bytes | Extra active Vulkan buffer bytes | Cache bookkeeping bytes across 45 pipelines |
| ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | 0 | 0 | 0 |
| 2 | 21,600 | 21,600 | 34,560 | 2,520 |
| 4 | 64,800 | 64,800 | 103,680 | 3,960 |
| 8 | 151,200 | 151,200 | 241,920 | 6,840 |

The Vulkan buffer increase comes from upload staging allocations retained with
the old textures. Bookkeeping counts the cache header and entry array, excluding
allocator overhead and bindgroup/backend storage. At steady state, imported or
fresh video retains 90/92/96 cache entries for capacities 2/4/8. The corresponding
cumulative CPU bindgroup allocations are 91/93/97, versus 90 without a cache.
Reused uploads retain 89 entries at every capacity.

Imported VAAPI textures are excluded from `ngpu_memory_stats.texture_bytes`:
only non-imported textures are accounted in `ngpu_texture_init()`. Consequently,
unchanged tracked memory on the hardware video path does **not** establish that
retaining additional imported textures has no cost. Decoder surface and driver
memory were not measured. The upload experiment measures that retention tradeoff
for ordinary textures, using a small video; larger textures cost more.

Every cache has zero entries at the first idle frame. After outstanding work
retires, every capacity and video path reaches the same tracked idle resource
level on the second idle frame:

| Backend | Buffer bytes | Texture bytes |
| --- | ---: | ---: |
| OpenGL | 292,160 | 9,540,608 |
| Vulkan | 8,680,772 | 9,771,012 |

## Correctness and conclusion

- Native cache tests passed on OpenGL, OpenGL ES and Vulkan: exact hits, LRU
  replacement, buffer-range differences, invalid inputs, references surviving
  eviction and clearing, and final resource reclamation.
- The pipeline regression passed on all three backends with caching enabled:
  changing metadata for identical textures, staging growth, and discard before
  submission followed by reactivation.
- Captured pixels matched the no-cache baseline for every one of 400 frames at
  capacities 2/4/8 on both measured backends, for normal video and both controlled
  upload cases. These capture checks were separate from the timing runs.

After promotion into the production implementation, all 1,488 integration tests
and all 18 library tests passed on the Radeon GPU across OpenGL, OpenGL ES and
Vulkan. The expanded bindgroup test covers complete-key differences, cache hits
while commands retain the object, LRU eviction, and clearing before submission.
The production cache also matched all 400 uncached reference frames on each of
OpenGL and Vulkan, and the tools build passed. This GPU validation exposed a
pre-existing nonzero-offset Vulkan staging-upload bug, corrected separately.

Two entries are the smallest successful capacity among those tested. Four and
eight provide no additional reuse here and retain more stale video resources.
The production pipeline uses a two-entry consumer cache. These results do not
justify a larger default, a claim of improved frame rate, or applying the same
policy to Android imports without checking their resource lifetime and retained
memory.

The prototype, runners, raw logs, summaries and mapper audit are under
`/tmp/bindgroups-benchmark/cache*`; the preparation, build and analysis scripts
are `/tmp/prepare-bindgroups-cache.py`, `/tmp/build-bindgroups-cache.py`,
`/tmp/run-bindgroups-cache.py`, `/tmp/run-bindgroups-cache-video.py`,
`/tmp/summarize-bindgroups-cache.py` and `/tmp/report-bindgroups-cache.py`.
