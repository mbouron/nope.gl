# nae — nope audio engine

A small C23 audio renderer for an NLE, built independently of the graphics
context. It provides scheduled PCM sources, multitrack mixing, gain ramps,
transactional graph replacement, custom processors, and optional nope.media
and miniaudio adapters.

The first version deliberately has three libraries:

| Target / pkg-config name | Responsibility |
| --- | --- |
| `nae` / `libnae` | Device-independent graph compiler and renderer |
| `nae-nopemd` / `libnae-nopemd` | Buffered, timestamped nope.media sources |
| `nae-miniaudio` / `libnae-miniaudio` | Playback device and format conversion through miniaudio |

The core does not depend on nope.gl, nope.media, miniaudio, or a thread library.
Public headers contain no dependency-specific types. All buffers use interleaved
float32, one sample rate and channel count per engine. The nope.media adapter
supports stereo; the core supports 1–8 channels with host-defined channel order.

## Build

With the repository's configured build environment:

```sh
make nae-install
make nae-tests
nae-play first.wav second.wav
```

`configure.py` downloads miniaudio **0.11.26**, verifies its checksum, and builds
it as a **separate PIC static library** through its upstream CMake project.
The device adapter links that archive normally. There is no miniaudio
implementation included in nae's source files. Its public symbols are hidden
in the repository build, and its configuration macros come from its installed
pkg-config file. A small external patch fixes upstream's generated static linker
flags. The core and adapters follow Meson's `default_library` setting.

This choice avoids treating miniaudio as a stable shared-library ABI. It has
public struct layouts and does not guarantee ABI compatibility between releases.
The adapter pins the dependency version and checks the runtime version as well;
all consumers of a separately supplied shared build must also use matching build
configuration macros. See [miniaudio's manual](https://miniaud.io/docs/manual/).

For a standalone core-only build:

```sh
meson setup builddir/nae libnae -Dnopemd=disabled -Dminiaudio=disabled
meson test -C builddir/nae --print-errorlogs
```

With `libnopemd.pc` and the separately installed `miniaudio.pc` on
`PKG_CONFIG_PATH`:

```sh
meson setup builddir/nae libnae -Dnopemd=enabled -Dminiaudio=enabled
meson compile -C builddir/nae
meson test -C builddir/nae --print-errorlogs
builddir/nae/nae-play --null first.wav second.wav
```

`--null` uses miniaudio's headless device; omit it for actual playback. The player
mixes files from time zero, supplies one fader per track plus a master fader, and
pumps decoders on its main thread. Production hosts can use their existing
worker pool with at most one simultaneous pump job per media instance.

The native Android build installs all three libraries. The AAR's CMake imports
and Prefab modules expose `nopegl::nae`, `nopegl::nae-nopemd`, and
`nopegl::nae-miniaudio`. Build the native environment before building the AAR.
NGL audio authoring nodes, JNI playback methods, and viewer clock integration
are separate work; this library does not start audio as a side effect of drawing.

## Graph and ownership

Create nodes on the control thread. A node is a persistent processor instance;
each reachable node is evaluated exactly once per block, including when its
output fans out. A custom processor may have multiple input buses (for example,
a compressor's program and sidechain), and one output bus. All processors must
currently have zero externally visible processing latency.

`nae_engine_commit()` takes an array of routes and an output index. An input is
another route's index, or `NAE_INPUT_NONE` for silence. Routes may be in any
order. The compiler checks references and duplicate instances, then traverses
the reachable graph to reject cycles and create a topological execution plan.
Compilation is O(nodes + input edges); buffers and input pointers are prepared
before publication. Unreachable nodes do not run. Limits are 4096 routes and
64 inputs per node.

```c
/* Existing source nodes a and b belong to engine e. Check every result. */
struct nae_node *gain_a = NULL, *gain_b = NULL, *mix = NULL;
nae_gain_create(e, 1.0f, 240, &gain_a);
nae_gain_create(e, 0.5f, 240, &gain_b);
nae_mix_create(e, 2, &mix);
uint32_t in_a[] = {0}, in_b[] = {1}, in_mix[] = {2, 3};
struct nae_route routes[] = {
    {a, NULL}, {b, NULL}, {gain_a, in_a}, {gain_b, in_b}, {mix, in_mix},
};
int ret = nae_engine_commit(e, routes, 5, 4);
/* If ret==NAE_AGAIN, retain/coalesce the desired edit and retry off-thread. */
```

Successful publication retains the reachable nodes and copies route arrays.
Adoption happens at the next processing boundary, including boundaries inside
a large callback. One transaction can be outstanding. The render thread
acknowledges the retired plan, and `nae_engine_collect()` releases it on the
control thread. `commit()` also collects before compiling. No destructor runs
in the callback.

Keep a caller-owned node reference while editing its parameters. Call
`nae_node_unrefp()` when it is no longer needed by the host; active/pending plans
keep their own references. A source's `user` pointer transfers to its node only
on successful creation. Stop and join rendering before destroying the engine,
and release caller-owned node references before destroying their engine.

Each engine has one serialized control thread and one renderer. Custom processor
state is exclusively owned by the renderer after publication. Do not inspect
or mutate it from the control thread. Preallocate it before creating its node;
its `process` and `reset` callbacks must be bounded and allocation-free.

## Editing and transport

`nae_gain_set()` updates a lock-free 32-bit target. The renderer picks it up at
a block boundary and ramps in linear amplitude over the configured number of
frames. Current gain/ramp state survives graph replacements when the same node
instance is reused. This is interactive smoothing, not an authored automation
curve. Hosts should convert dB to linear gain before calling it.

`nae_source_set_range()` changes a source's authoring range. The next commit
snapshots the new range; an already published plan remains unchanged. Clip
intersections are computed in samples, including boundaries within a block.
This is a normal-speed source mapping; speed changes and time stretching are
not implemented.

`nae_engine_render(e, first_frame, frames, output)` writes the entire requested
range, splitting it into blocks no larger than the configured maximum. The
caller owns transport. A discontinuity resets processor history; normal
playback and graph replacements preserve it. Positions are nonnegative signed
64-bit sample indices. Rendering does not depend on video frame timing.

The renderer preserves float headroom. The device sink clamps only the final
output and converts non-finite output samples to silence. A plan replacement is
memory-safe but is not inherently click-free: fader ramps handle gain changes;
arbitrary routing transitions need host-defined fades or crossfades.

## Decoding, underruns and seeks

Create one `nae_media` per simultaneous playback cursor. It owns one nope.media
context and a fixed, power-of-two queue of PCM blocks. The adapter selects raw
stereo float audio and requests conversion to the engine rate using
`aresample`. It never calls nope.media or frees decoded frames from the renderer.

`nae_media_pump()` publishes at most one queue block per call, returning
`NAE_AGAIN` if full. It may block decoding a frame. After EOF it stays at EOF
until explicitly sought, avoiding nope.media's automatic restart on a subsequent
`get_next_frame()` call. The feeder and audio callback may run concurrently.
The callback snapshots queue occupancy so its work remains bounded even while
the feeder publishes more PCM.

Missing data is silent and yields `NAE_AGAIN` from rendering. Other tracks keep
playing. Queued PCM carries source positions: late data is skipped rather than
played late. Decode failures are reported by the pump and propagated by the
source once its queued data has been consumed.

Seeking a media source currently requires a **quiescent feeder and renderer**:

1. Stop the sink (which waits for callbacks), and stop/join pending pump jobs.
2. Seek each active media instance to its required source position.
3. Prefill its queue, and set the stopped sink's project position.
4. Restart feeding and playback.

Do not reset queues or call `nae_media_seek()` while either side is active.
An asynchronous generation-based seek protocol can be added later. For export,
prefill before each small render request and treat unexpected missing data as
an error; do not retry a partially processed block with stateful effects.
Independent playback/export runs need separate engines and processor state.

The sink exposes source underrun counts and device-stop/error status. It does
not expose an audible presentation clock or automatically repair device
disconnects. The host owns device recreation and Android audio-focus/lifecycle
handling. The example's fixed drain allowance is not an A/V synchronization API.

## Validation and expansion

Tests cover clip boundaries and trims, graph ordering/fan-out, cycle rejection,
gain continuity, range snapshots, retirement/destruction, concurrent graph
replacement, raw PCM decoding, source-rate conversion, underrun recovery,
sample-level seeks, EOF latching, and miniaudio's null sink lifecycle.

The extension point is `nae_processor`: a preallocated state object with
process/reset/destroy hooks and multiple input buses. EQ and simple sidechain
ducking can fit this interface. Before adding lookahead, convolution, or
arbitrary plugins, add explicit latency reporting and compensation, tail policy,
automation events, and channel-layout negotiation. GPU processing, plugin
hosting, recording, and DSP parallelism are outside this first version.
