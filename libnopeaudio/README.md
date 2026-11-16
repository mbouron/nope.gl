# nope.audio

## Introduction

`nope.audio` is a C23 library for rendering audio tracks on an editing
timeline, in real time or offline. It is independent of the graphics context.

It builds as `nopeaudio` (pkg-config `libnopeaudio`):

| Part | Role | Built |
| --- | --- | --- |
| Core | Engine (graph of nodes) and transport | Always |
| Media sources | Audio decoded with nope.media | With the `nopemedia` option |
| Sink | Audio output through SDL3, Oboe or a null device | Always |
| Player | Media tracks on a timeline, WAV export | With nope.media |

`NAU_HAVE_NOPEMEDIA` indicates whether media sources and the player are
available. The `sink` option selects one backend. With `auto`, Oboe is used
if available, then SDL3, then null. Null devices render in real time without
audio output and are available with every backend.

## License

Apache 2.0, see [LICENSE](../LICENSE).

## Compilation, installation

From the `libnopeaudio` directory:

```sh
meson setup builddir
meson compile -C builddir
meson install -C builddir
```

`meson configure` lists the available options. See the [Meson
documentation][meson-doc] for more information. Use
`-Dnopemedia=disabled -Dsink=null` to build without media or device libraries.
Shared and static libraries require separate build directories, selected
with Meson's `default_library` option.

WAV export requires FFmpeg's `libavformat`, `libavcodec`, `libavutil` and
`libswresample`, with the WAV muxer, file protocol, and `pcm_s16le` and
`pcm_f32le` encoders enabled.

[meson-doc]: https://mesonbuild.com/Quick-guide.html#compiling-a-meson-project

From the repository root, `make nopeaudio-install` builds the library in the
configured environment. `configure.py` uses the same SDL3 as `ngl-tools` on
desktops, builds SDL3 for iOS, and builds Oboe for Android.


## API Usage

For API details, see [nopeaudio.h](src/include/nopeaudio/nopeaudio.h.in).
The examples below omit error handling.

Objects are allocated with `*_create()` and configured with `*_init()`.
Initialization requires an uninitialized object and can be retried on
failure. `*_freep()` and `*_unrefp()` also accept uninitialized objects and
clear the caller's pointer. Tracks and track effects belong to their player
and use its add/remove functions.

Audio is interleaved 32-bit float. The core supports up to 8 channels;
media sources and the player are stereo. Positions and durations are in frames
at the engine's sample rate, and timestamps are in nanoseconds.

### Threads

The control thread builds graphs and controls playback. Its calls must be
serialized. Each media has a decoding thread, in addition to nope.media's
own threads. Rendering pulls audio through the layers:

```
device callback --> sink --> transport --> engine --> sources
                             playback      graph      decoded audio
                             and clock     of nodes   queued ahead
```

In real time, the render thread must not block, allocate or free memory.
Graph edits are published without waiting for it. An edit returns
`NAU_AGAIN` if the previous one is still pending; retry later. Failed edits
leave the current graph unchanged. Opening media and stopping devices can
block the control thread. Offline rendering can wait for decoding.

### Engine

An engine renders sources, gains, faders, effects, mixes and custom
processors. Create nodes with `nau_node_create()`, then use a built-in
initializer or `nau_node_init()` for a custom processor.

```c
const struct nau_engine_config config = {
    .sample_rate = 48000, .channels = 2, .block_frames = 256,
};
struct nau_engine *engine = nau_engine_create();
nau_engine_init(engine, &config);

struct nau_node *gain = nau_node_create();
nau_gain_init(gain, engine, 1.f, 240);

/* source --> gain (output); see "Media sources" for source */
const uint32_t gain_inputs[] = {0};
const struct nau_route routes[] = {
    {.node = source},
    {.node = gain, .inputs = gain_inputs},
};
nau_engine_commit(engine, routes, 2, 1);
```

The engine prepares routing, buffers and configuration copies on the control
thread. The render thread adopts the graph at a block boundary and keeps
processor state across commits. Replaced graphs are freed by the control
thread on the next commit, or by `nau_engine_collect()`.

Fader and effect automation is relative to each node's origin
(`nau_node_set_origin()`). Moving a clip changes the origin without changing
its ramps. Faders evaluate volume at every frame; effects evaluate parameters
every 16 frames relative to this origin. Splitting render calls does not
shift the automation.

Sources play at normal speed; time stretching, effect latency compensation
and tail handling are not implemented. A low-pass filter keeps ringing after
the end of its input. Routing changes can cause an audible discontinuity; the
host is responsible for fading around them.

### Effects

`nau_effect_get_info()` describes each effect's parameters: name, range,
default value and interpolation scale (linear, logarithmic or stepped).
Effect types are numbered from 0 without gaps; enumeration ends when the
function returns `NULL`. The initial effect, `NAU_EFFECT_PASSTHROUGH`, leaves
its input unchanged and has no parameters. An effect with a sidechain (the
ducker) has a second input, its key.

`nau_effect_set_param()` sets a target from any thread. The effect ramps
towards it, except for stepped parameters, which change immediately.
`nau_effect_set_automation()` replaces a parameter's ramps at the next graph
commit. Automation starts from the current parameter value and follows its
scale: for example, logarithmic frequency changes move by constant ratios.

### Transport

A transport adds playback controls and an audio clock. Call
`nau_transport_render()` instead of `nau_engine_render()`.

```c
const struct nau_transport_config config = {
    .declick_frames = 240,          /* 5 ms at 48 kHz */
    .ready_timeout_frames = 12000,  /* 250 ms at 48 kHz */
};
struct nau_transport *transport = nau_transport_create();
nau_transport_init(transport, engine, &config);
nau_transport_set_duration(transport, length);
nau_transport_play(transport);

/* Synchronize video with the audio currently heard. */
int64_t position;
if (nau_transport_get_position_at(transport, nau_sink_now(), &position) == NAU_OK) {
    /* Draw the video frame at position. */
}
```

Playback changes fade through silence to avoid clicks. When playback starts
from silence (play, seek, loop wrap), the position holds until active sources
are ready or the readiness timeout expires. This preserves the first frames
while decoding catches up. Sources without a `ready` callback are considered
ready.

When rendering resumes after a pause, readiness wait or loop hold,
processors are reset without seeking the sources. Gain and parameter targets
set while idle then apply immediately, since output resumes from silence.

Paused render calls still adopt commits and position sources.
`nau_transport_sync()` does this when rendering is stopped; it must never
run concurrently with rendering. Set `declick_frames` to 0 to disable fades
for offline rendering.

The clock smooths callback timestamps with a delay-locked loop and keeps
the last 256 playback segments. Clock queries retry concurrent writes and
return `NAU_AGAIN` if they cannot get a consistent result.

### Media sources

A media decodes a file in stereo at the engine's sample rate.

```c
struct nau_media *media = nau_media_create();
nau_media_init(media, engine, "music.m4a", NULL);
const int64_t length = nau_media_get_length(media);

struct nau_node *source = nau_node_create();
nau_media_init_source(media, source, 0, 0, length);
```

A media has one source node. Playing the same file in several clips requires
one media per clip. Decoding runs 250 ms ahead; missing audio is replaced
with silence and skipped when it arrives to preserve synchronization.
Gaps in the timestamps of the decoded audio and the end of the media do not
hold playback.

Loudness normalization needs 2.25 s of history. After a seek, decoding starts
that much earlier and discards the pre-roll so the level approaches that of
continuous playback. Timestamp gaps shorter than 2 ms and overlaps are
joined. Decoder errors are reported separately from the end of the media,
which is remembered until the next seek.

### Sinks

A sink renders a transport on an audio device.

```c
nau_sink_backend_init();                /* main thread */

struct nau_sink *sink = nau_sink_create();
nau_sink_init(sink, transport, NULL);
nau_sink_start(sink);

while (nau_transport_is_playing(transport)) {
    nau_sink_poll();                    /* main thread */
    if (nau_sink_get_status(sink) != NAU_OK)
        break;
    /* ... */
}

nau_sink_freep(&sink);
nau_sink_backend_uninit();              /* main thread */
nau_transport_freep(&transport);
nau_node_unrefp(&gain);
nau_node_unrefp(&source);
nau_engine_freep(&engine);
nau_media_unrefp(&media);
```

The host manages audio focus and application lifecycle. Timing estimates
still need validation on physical devices for each platform.

#### SDL

- Balance successful `nau_sink_backend_init()` calls with
  `nau_sink_backend_uninit()`. The last uninitialization returns `NAU_AGAIN`
  while device sinks exist. The library holds one reference to SDL audio
  and leaves other subsystems alone.
- Call `nau_sink_poll()` regularly on the main thread, even if the application
  pumps SDL events itself. It may wait for a device callback to finish.
- Sinks can be controlled from another thread. Threads not created by SDL
  must call `SDL_CleanupTLS()` before exiting after using SDL.
- If the output becomes unavailable, polling pauses the sink and reports an
  error. It resumes playback once SDL moves the stream to a usable output.
  If recovery fails, stop and restart the sink. Rendering errors persist
  until the next start.
- SDL can continue reporting a lost default output as available. Playback
  then continues silently until SDL selects another output or the sink stops.
  SDL before 3.4.18 can use freed memory when moving several streams to a
  new default output.
- SDL exposes neither presentation timestamps nor backend buffering. Latency
  is estimated from queued audio and two device buffers: one being filled,
  one playing.

#### iOS

SDL opens the audio session with the Playback category, mixing with and
ducking other applications. Set `SDL_AUDIO_CATEGORY` before
`nau_sink_backend_init()` to choose another category, such as `playandrecord`.
SDL deactivates the session when its last device closes; stopping a sink
closes its device.

#### Android

The sink uses a shared low latency AAudio stream (OpenSL ES before Android
8.1). After a disconnection, it reopens on the new output.

The build includes media sources, the Oboe sink and the player. CMake and
Prefab expose `nopegl::nopeaudio`; build the native environment before the AAR.

The Kotlin binding, `org.nopeforge.nopeaudio.AudioPlayer`, validates track
ownership and invalidates handles when tracks or effects are removed.
`effectTypes` lists the native effect descriptions. Edits return `AGAIN`
while a previous edit is pending. `addTrack()` returns a `TrackResult` with
a track only on `OK`; `getPositionAt()` returns `null` when unavailable.

Call `org.nopeforge.Nope.init(context)` before opening `content://` URIs.
`NGLContext.init()` also performs this initialization.

### Player

A player combines the layers into media tracks. Each track has volume
automation (fader) and an adjustable gain (volume), with effects before or
after both controls:

```
source --> [effects] --> fader --> volume --> [effects] --> mix
```

Any effect can use either placement. For example, a compressor after these
controls responds to volume changes; before them, the controls only change
its output level.

```c
const struct nau_player_config config = {.mode = NAU_PLAYER_DEVICE};
struct nau_player *player = nau_player_create();
nau_player_init(player, &config);    /* after nau_sink_backend_init() */

const struct nau_track_params params = {
    .filename = "music.m4a", .duration = -1, .volume = 1.f,
};
struct nau_track *track = NULL;
nau_player_add_track(player, &params, &track);
nau_player_set_duration(player, nau_track_get_duration(track));
nau_player_play(player);
```

Track automation is relative to the track's start and moves with it.
Effect targets change without a graph commit. All lanes passed to
`nau_player_set_track_effect_automation()` take effect together. Effect
state survives automation edits and moves; changing effect order or ramp
values can still cause an audible discontinuity.

A track heard under water, then back at the surface:

```c
const int64_t rate = nau_player_get_sample_rate(player);
struct nau_track_effect *lowpass = NULL;
nau_player_add_track_effect(player, track, NAU_EFFECT_LOWPASS,
                            NAU_TRACK_EFFECT_PLACEMENT_PRE_FADER, &lowpass);
const struct nau_ramp cutoff[] = {
    /* At 2 s: down to 500 Hz in 0.3 s */
    {.frame = 2 * rate, .length = rate * 3 / 10, .value = 500.f},
    /* At 6 s: open again in 1.5 s */
    {.frame = 6 * rate, .length = rate * 3 / 2, .value = NAU_LOWPASS_OPEN},
};
const struct nau_lane lane = {NAU_LOWPASS_CUTOFF, cutoff, 2};
nau_player_set_track_effect_automation(player, lowpass, &lane, 1);
nau_player_set_track_effect_param(player, lowpass, NAU_LOWPASS_RESONANCE, 1.4f);
```

Automating the resonance with the cutoff takes a lane for each, in one call.
A slider can drive the depth of the filter (`NAU_LOWPASS_AMOUNT`) on top of
its automation.

Music ducking under two voices:

```c
struct nau_track_effect *ducker = NULL;
nau_player_add_track_effect(player, music, NAU_EFFECT_DUCKER,
                            NAU_TRACK_EFFECT_PLACEMENT_POST_FADER, &ducker);
nau_player_set_track_effect_param(player, ducker, NAU_DUCKER_THRESHOLD, 0.01f);  /* voices above -40 dB */
nau_player_set_track_effect_param(player, ducker, NAU_DUCKER_VOLUME, 0.25f);     /* down to -12 dB */
struct nau_track *const voices[] = {voice, other_voice};
nau_player_set_track_effect_sidechain(player, ducker, voices, 2);
```

A sidechain hears its key tracks after their volume, before their effects
placed after it: a silent track ducks nothing, and tracks can duck under
each other with effects placed after their volume. A track listening to
itself through effects placed before the faders is refused.

With `NAU_PLAYER_HOST`, bracket rendering with
`nau_player_start_rendering()` and `nau_player_stop_rendering()`. The host
owns the rendering thread: `nau_player_stop()` pauses and seeks to the start
without stopping that thread.

With `NAU_PLAYER_OFFLINE`, serialize render calls with edits and seeks.
`nau_player_export(player, "mix.wav", NAU_EXPORT_S16)` exports the whole
project once, ignoring loop settings. Large files use RF64 and require a
reader with RF64 support.


## Development

Effect implementations live in `src/effect/`. Each supplies a
`struct nau_effect_class` and is registered in `effect.c`. The shared code
handles parameter targets, ramps and configuration snapshots. The tests check
every effect type: its description, and that its output depends neither on
how the renders are split nor on where its track is.

The private exporter in `src/export/export.h` accepts arbitrary chunks of
interleaved stereo float, converts them to the encoder's sample format and
buffers complete encoder frames. Input and output share the sample rate.
`nau_player_export()` configures it for WAV with PCM encoding and automatic
RF64 support. Closing the exporter drains the encoder, finalizes the file
and frees its resources, including after a failed open.

### Running tests

From the `libnopeaudio` directory, run `meson test -C builddir -v`. From the
repository root, `make nopeaudio-tests` builds and runs them in the configured
environment. The test programs are in the build directory, along a sample
player tool named `nopeaudio-play` (if its dependencies were met at build
time), which can be used for other manual testing purposes:

```sh
builddir/nopeaudio-play --null first.wav second.wav
```

The tool mixes its files from the start of the project. `--null` selects the
null device.

### Validation

The tests cover the graph, the ramps, every effect type, the filter
responses, the ducker, the effect chains and their sidechains, the block
boundaries, seeks, short loops, late clock queries, concurrent edits,
decoding, the queue between threads, the error paths, the export and the null
device. ASan/UBSan and TSan use separate Meson builds.

On Linux, the player tests also check the voluntary context switches of the
render thread. This measures neither the callback deadlines nor the latency of
the hardware.

The Oboe tests exercise its failures on desktops, against a mock of its API.
The SDL tests use its dummy driver.

The WAV tests cover PCM samples and file lifecycle; the generic exporter
tests also exercise FLAC in FLAC/Matroska and AAC in MP4 when supported by
the FFmpeg build.
