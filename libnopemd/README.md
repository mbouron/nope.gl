# nope.media

## Introduction

`nope.media` is a video playback library for fetching frames at exact times.

It can handle only one stream at a time, generally video. If audio is
requested, the frame returned will be an instant video frame containing both
amplitudes and frequencies information.

## License

LGPL v2.1, see `LICENSE`.

## Compilation, installation

```sh
meson setup builddir
meson compile -C builddir
meson install -C builddir
```

`meson configure` can be used to list the available options. See the [Meson
documentation][meson-doc] for more information.

[meson-doc]: https://mesonbuild.com/Quick-guide.html#compiling-a-meson-project


## API Usage

For API details, see [nopemd.h](src/nopemd.h.in).

### Looping over part of a media file

Use `nmd_set_loop_range()` to prepare frames from the next repetition while
the current one is still playing:

```c
nmd_set_loop_range(player, 2.0, 5.0);
```

The range includes 2 seconds and excludes 5 seconds. The application controls
the playback clock and wraps the times passed to `nmd_get_frame()`, for example
`2.0, 2.1, ..., 4.9, 2.0, 2.1, ...`. During looping, a request earlier than
the last returned frame and inside the range advances to the next repetition.
Use `nmd_seek()` for an explicit change of playback position, such as scrubbing
backward.

Loop times are relative to the `start_time` option, just like frame requests.
With `start_time` set to 10 seconds, the example loops over media times
`[12s, 15s)`. If the loop end extends beyond the media, requests past the last
frame keep showing that frame until the application wraps its clock.

You can change the range during playback. The player uses queued frames when
possible and seeks if the requested frame is no longer available.
Call `nmd_set_loop_range(player, 0.0, 0.0)` to disable looping. If filters
remove every frame of a complete repetition, playback ends. After a range
change, the next `nmd_get_frame()` call seeks and tries again.

The demuxer seeks back to the range start in advance, while frames from the
current repetition remain queued for playback. These internal loop seeks do
not trigger the callback registered with `nmd_set_seek_callback()`.


## Development

### Running tests

Assuming `nope.media` is built within a `builddir` directory (`meson builddir
&& meson compile -C builddir`), the test can be run with meson using a command
such as `meson test -C builddir -v`.

The testing program itself (`test-prog`) can be found in the build directory,
along a sample player tool named `nope-media` (if its dependencies were met at
build time), which can be used for other manual testing purposes.

### Infrastructure overview

```
              .                             .
              .             API             .
              .                             .
              . . . . . . . . . . . . . . . .
              .                             .   .------------.               .-----------------------------------------------------.
              .                             .   | api.c      |               | control                                             |
              .                             .   +------------+               +-----------------------------------------------------+
           ===.===  nmd_create()      ======.==>|            |===  init  ===>|                                                     |
              .                             .   |            |---  start  -->|                    MANAGE THREADS                   |
           ===.===  nmd_get_*frame()      ==.==>|            |---  seek  --->|              _________/  |  \_______                |
  USER        .                             .   |            |---  stop  --->|             /            |          \               |
           ---.---  nmd_start()      -------.-->|            |===  free  ===>|            /             |           \              |
           ---.---  nmd_seek()       -------.-->|            |               |           v              v            v             |
           ---.---  nmd_seek()       -------.-->|            |               |      .----------.  .----------.  .----------.       |
              .                             .   |            |               |      | demuxer  |  | decoder  |  | filterer |       |
           ===.===  nmd_free()       =======.==>|            |               |      +----------+  +----------+  +----------+       |
              .                             .   |            |               |      | init     |  | init     |  | init     |       |
              .                             .   `------------'               |      +----------+  +----------+  +----------+       |
              .                             .                                |      |          |  |          |  |          |       |
              .                             .                                |      |  RUN     |  |  RUN     |  |  RUN     |       |
              .                             .                                |      |          |  |          |  |          |       |
                                                                             |      |          |  |          |  |          |       |
                                                                             |   O==|==O    O==|==|==O    O==|==|==O    O==|==O    |
                                                                             |      |          |  |          |  |          |       |
                                                                             |      +----------+  +----------+  +----------+       |
                                                                             |      | free     |  | free     |  | free     |       |
                                                                             |      `----------'  `----------'  `----------'       |
                                                                             |                                                     |
                                                                             |                                                     |
                                                                             |   O=====O in control queue                          |
                                                                             |                                                     |
                                                                             |   O=====O out control queue                         |
                                                                             |                                                     |
                                                                             `-----------------------------------------------------'

O====O       message queue
-- xxx -->   async operation
== xxx ==>   sync operation
```
