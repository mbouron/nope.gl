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
mixes files from time zero, with one gain per track plus a master gain; each
media decodes on a thread of its own.

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

`nae_fader_create()` follows volume keyframes over the project frames: from
each keyframe's frame, a linear ramp from the previous keyframe's volume (unity
before the first) over its length, then that volume. Keyframes are snapshotted
by the next commit, as source ranges are, so automation edits are as safe as
graph edits.

## Transport

`nae_transport` plays the project from the render thread: the device callback
calls `nae_transport_render()` instead of `nae_engine_render()`. Play, pause,
seek, duration, looping, the loop range and the hold at the end of each pass
are set from the control thread, wait-free, and applied by the next render.
Without looping, playback stops at the end of the project and goes back to
its start.

Every jump is declicked: a seek is adopted once the output has faded to
silence, a loop pass fades out into its end and back in from its start, and
pause and play fade. After a jump the output stays silent until the sources
playing have their data (a render answering other than `NAE_AGAIN`), or until
the ready timeout, while the position goes on: a slow source costs a few
milliseconds of silence, never a desync. Offline rendering uses no ramps.

Sources follow the jumps through their optional `seek` callback, called on the
render thread with the source frame read next, and before the first read. A
loop going round is flagged as a wrap, so a source decoding the loop ahead (the
nope.media adapter does) moves onto its next pass rather than seek. A seek
while paused, a hold at the end of a pass, and the end of the project position
the sources at once, for them to get their data before playback goes on.
Paused or holding still, the renders adopt the commits all the same, their new
sources positioned there: an edit never waits for playback to go on. Fading out
into a pause, they adopt none: an editor pausing for its edits is not heard
cutting its tracks.

Each render publishes a clock: the project frame heard at a host time, the
times of the renders smoothed by a delay-locked loop so the callbacks' jitter
does not reach it. `nae_clock_position_at()` extrapolates it, for a video
renderer to draw the frame heard at its vsync.

## Decoding, underruns and seeks

Create one `nae_media` per simultaneous playback cursor. It owns one nope.media
context, which a thread of its own decodes ahead of the renderer (250 ms), in
stereo float at the engine rate (optionally loudness-normalized). The renderer
never waits for it, takes no lock and frees nothing: the decoded chunks reach
it through a lock-free queue, and go back to the decoding thread to be
released.

Audio not decoded in time plays as silence (the source read answers
`NAE_AGAIN`), and the audio then late is skipped: a media stays in time. A
read from another position than the one read next, as the transport's seeks
do through the source `seek` callback, is a request the decoding thread serves
while the renderer goes on: the chunks decoded for an earlier request are
dropped. A request is decoded ahead even while its source is not read, as when
paused, scrubbing or before its range starts: the audio queued for the
positions left goes back at once. Chunk timestamps off by less than 2 ms from
the previous chunk's end are taken as contiguous, so imprecise timestamps leave
no hole.

`nae_media_set_loop()` has nope.media go round a range of the media ahead of
the renderer. The decoding thread numbers the passes, and the source moves
onto the next one when the transport wraps: going round a loop never seeks.
Audio decoded past the end of the loop before nope.media knew of it is
dropped, for it not to take the room of the next pass. A short media, read to
its end as soon as it is opened, goes round a loop set afterwards all the same.
A source positioned at the end of its loop, as one past the end of its range
is, is decoded from the start of the loop, for the pass the transport wraps to.

Past the end of the media a source plays silence (`NAE_EOF`) without asking
the decoder again. For offline rendering, a blocking media has the reads wait
for their data, so an export is complete and deterministic.

The sink plays a transport when given one: the device then runs on while
paused, rendering silence, so a seek still reaches the sources at once, and
each callback times the transport's clock with the time its first frame is
heard (now plus the latency of the device's buffering, in `nae_sink_now()`'s
monotonic time base, Android's `System.nanoTime()`). The sink exposes source
underrun counts and device-stop/error status. It does
not repair device disconnects. The host owns device recreation and
Android audio-focus/lifecycle handling.

## Player

`nae-player` (`nae/player.h`) plays tracks of media files laid on a project,
as an editor plays the audio of its timeline: each track plays a part of its
media (`start`, `trim`, `duration`) through its fader (volume keyframes from
the start of the track) and volume, mixed under a master volume. Tracks are
added, moved, faded and removed while playing: each edit is a commit of the
graph, adopted by the next callback. Play, pause, seek, the duration,
looping, the loop range and its hold go to the transport, and the loop range
to the media of the tracks, which decode the part of it they play ahead.

It plays on the default device, a headless one, a host's rendering thread
(`NAE_PLAYER_HOST`), or offline, where `nae_player_record()` writes the
project to a 16-bit or float WAV file. The player also reports the position
heard at a time, and the underruns and decoder statistics of its tracks.

The control functions are serialized and none waits for the audio thread;
its test counts the context switches of a thread rendering in real time
while the control thread edits, seeks and loops, and requires none.

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
