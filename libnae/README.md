# nae

C23 audio rendering for an editing timeline, independent of the graphics context.

| Target / pkg-config name | Role |
| --- | --- |
| `nae` / `libnae` | Graph compiler, renderer and transport |
| `nae-nopemd` / `libnae-nopemd` | Buffered nope.media sources |
| `nae-miniaudio` / `libnae-miniaudio` | Playback device and format conversion |
| `nae-player` / `libnae-player` | Media tracks, timeline controls and WAV export |

The core has no dependency on nope.gl, nope.media, miniaudio or a thread library.
Buffers contain interleaved float32 at one sample rate and channel count per
engine. The core supports 1–8 channels with host-defined channel order; the
nope.media adapter and player use stereo.

## Build

With the repository's configured environment:

```sh
make nae-install
make nae-tests
nae-play first.wav second.wav
```

`configure.py` builds miniaudio 0.11.26 as a separate PIC static library after
verifying its checksum. The sink links that archive with the configuration
macros from its pkg-config file. The core and adapters follow Meson's
`default_library` setting.

For a core-only build:

```sh
meson setup builddir/nae libnae -Dnopemd=disabled -Dminiaudio=disabled
meson test -C builddir/nae --print-errorlogs
```

With `libnopemd.pc` and `miniaudio.pc` on `PKG_CONFIG_PATH`:

```sh
meson setup builddir/nae libnae -Dnopemd=enabled -Dminiaudio=enabled
meson compile -C builddir/nae
meson test -C builddir/nae --print-errorlogs
builddir/nae/nae-play --null first.wav second.wav
```

`--null` selects the headless device. The example mixes files from time zero.

The Android build installs all four libraries. CMake and Prefab expose
`nopegl::nae`, `nopegl::nae-nopemd`, `nopegl::nae-miniaudio` and
`nopegl::nae-player`. Build the native environment before the AAR. The Kotlin
binding is `org.nopeforge.nae.AudioPlayer`; drawing does not start playback.

## Graph and ownership

An engine has one serialized control thread and one renderer. Create nodes,
edit authoring parameters and publish routes on the control thread.
`nae_engine_commit()` copies the routes, retains reachable nodes and prepares
all render buffers before publication. Routes may be unordered; cycles,
duplicate instances and foreign nodes are rejected. Each reachable node runs
once per block, including when its output feeds several inputs. Limits are
4096 routes and 64 inputs per node.

Only one commit can be pending. `NAE_AGAIN` leaves the active graph unchanged;
retry after the renderer adopts the pending commit at a block boundary.
`nae_engine_collect()` releases retired plans on the control thread. Commit
also collects; no node destructor runs in the callback.

Keep a node reference while editing it. `nae_node_unrefp()` releases that
reference; active and pending plans retain their own. A source's `user`
pointer transfers to its node only on successful creation. Stop and join
rendering, then release node references before freeing the engine.

Custom processor state belongs to the renderer after publication. Its process
and reset callbacks must be bounded and allocation-free. Processors currently
have no externally visible latency. Effects with lookahead or tails require
latency compensation and a tail policy before they can use this interface.

## Sources and automation

Source ranges map project frames to media frames at normal speed.
`nae_source_set_range()` edits an authoring range; the next successful commit
snapshots it. Published plans retain their previous ranges. Speed changes and
time stretching are not implemented.

`nae_gain_set()` publishes an atomic linear-amplitude target. The renderer
ramps towards it over the configured number of frames, preserving ramp state
across graph changes. Hosts convert dB to linear amplitude.

A fader follows authored volume keyframes. Each point ramps from the previous
volume (unity before the first point) to its target over its length, then holds
that target. Keyframes are copied and snapshotted by commit, like source ranges.

Rendering preserves float headroom. The device sink clamps the final output
and replaces non-finite samples with silence. Arbitrary routing changes need
host-defined fades or crossfades.

## Transport and clock

`nae_transport_render()` renders from the transport's position. Play, pause,
seek, duration and loop controls are applied by the renderer. Without looping,
playback stops at the project end and returns to its start. A loop can have a
silent hold at its end, during which the project position stays fixed.

Play, pause and jumps use gain ramps. Ramps are limited to half the playback
range, so short loops remain audible. After a jump, playback stays silent until
the active sources are ready or the ready timeout expires. The position keeps
advancing during this wait. Offline rendering disables these ramps.

Sources receive seek callbacks before their first read and after jumps. A loop
wrap is distinguished from a seek so a decoder can consume the next buffered
pass. Paused and holding renders still adopt graph edits and position sources.
`nae_transport_sync()` does the same while the renderer is stopped; it must
never run concurrently with rendering.

Pass the host time when the first frame of each callback will be heard, in
nanoseconds. The sink supplies this from its monotonic clock and estimated
buffering latency. A delay-locked loop smooths callback timing.

`nae_transport_get_position_at()` looks up audible position across buffered
seeks, loops, holds and pauses. History retains 256 rendered segments. It
returns `NAE_AGAIN` if the requested history is unavailable or a consistent
snapshot cannot be read. `nae_transport_get_clock()` returns the latest segment;
`nae_clock_position_at()` extrapolates that segment without accounting for
other buffered discontinuities.

## Decoding

Each `nae_media` owns a nope.media context and a decoding thread. Create one per
simultaneous playback cursor. It decodes 250 ms ahead in stereo float at the
engine rate, optionally with loudness normalization. nope.media also owns
worker threads; track count is not a decoder-thread budget.

Two SPSC queues in `src/spsc.h` transfer decoded chunk descriptors to the
renderer and return them for release on the decoding thread. Samples remain
owned by nope.media. Rendering takes no mutex and frees no chunks.

Unavailable data yields silence and `NAE_AGAIN`. Late samples are skipped to
keep the source in time. Seeks request a new position without waiting for the
decoder; chunks from older requests are discarded. Timestamps within 2 ms of
the previous chunk's end are treated as contiguous. EOF is latched until a
new request. Open and decode failures return an error instead of EOF.

`nae_media_set_loop()` lets the decoder buffer successive passes of a media
range. A transport wrap consumes the next pass without seeking. Blocking media
reads wait for data and are reserved for offline rendering.

## Player

`nae/player.h` provides up to 64 media tracks. Each track maps `start`, `trim`
and `duration` through its fader and volume into a mix with master volume.
Fader times are relative to the track start. Successful track edits publish a
graph; failed edits, including `NAE_AGAIN`, leave the player unchanged.

Serialize control calls. Media opening, recording, device stopping and decoder
teardown may block. Rendering must have a single owner:

- Device modes own their callback thread.
- In host mode, call `nae_player_start_rendering()` before starting the host
  thread. Stop and join it before `nae_player_stop_rendering()` or destruction.
  `nae_player_stop()` pauses and seeks; the host thread continues rendering.
- Offline mode renders synchronously. `nae_player_record()` writes the full
  project once as S16 or F32 WAV, ignoring preview loops and holds. It restores
  loop settings and leaves playback paused. WAV sizes exceeding RIFF limits
  are rejected before opening the output.

A track handle is valid until successful removal, clear or player destruction.
The Kotlin binding checks ownership and invalidates handles. `addTrack()`
returns a `TrackResult` containing a status and a track only on `OK`. Edits
return `AGAIN` while a previous graph is pending. `getPositionAt()` returns
`null` when audible-position history is unavailable.

Device disconnection, Android audio focus and lifecycle handling belong to the
host. The sink reports device errors but does not recreate the device.

## Validation

Tests cover graph validation and retirement, automation, sample boundaries,
seeks, short loops, delayed clock queries, concurrent edits, decoding, SPSC
wraparound, error paths, export and the null device. ASan/UBSan and TSan use
separate Meson builds. Linux player tests also check voluntary context switches;
this does not measure callback deadlines or hardware output latency.
