# Audio engine review handoff

Reviewed `nae-player` against `main` (`7bb8bb4b3`). The original branch head was
`35c2ca41c`. Fixes are separate commits after that head. This review covers the
core, transport, media adapter, sink, player, Android binding, build and tests.

Fixes for the reported correctness defects are committed. Android
runtime validation, decoder scaling and device timing remain open. Do not treat
the desktop sanitizer results as approval for Mojo production use.

## Findings and fixes

1. **P1 — Sink accepts a transport from another engine.** A mono sink attached
   to a stereo transport writes past the output buffer; reproduced with ASan.
   `04ce1ee99` checks engine identity before device initialization and documents
   the lifetime requirement. `test_sink.c` covers rejection.

2. **P1 — Host stop races with rendering.** `nae_player_stop()` called transport
   sync while the host callback was using render state; reproduced with TSan.
   First-render ownership was also implicit. `18ea27f82` adds explicit host
   start/stop rendering calls and avoids sync while rendering is active.
   `test_player.c` repeatedly stops and starts playback with a live host thread.

3. **P1 — Kotlin track handles outlive native tracks.** Remove, clear and release
   left raw pointers usable by getters and setters. Foreign handles and native
   address reuse were not rejected. `5a35670d6` tracks ownership, invalidates
   handles and serializes instance access. It also preserves native add status,
   including `AGAIN`. Android tests cover removal, clear, release and foreign
   handles; they compile but still need a device run.

4. **P1 — Media failures become successful silence.** A missing file could be
   added and exported without an error. Creation ignored start/info errors;
   read errors became EOF. `072743a73` checks initial operations and distinguishes
   decoder failure from EOF in the chunk queues. `test_media.c` covers a missing
   file. Later seek/restart/loop control errors still need the follow-up below.

5. **P1 — Audible clock skips buffered history.** Queries before the latest
   callback's future audible time were clamped to that callback's first frame.
   A simple case returned frame 480 instead of 240. One clock snapshot also
   cannot represent buffered seeks, loops, holds and pauses. `86fb514e1` retains
   256 rendered segments and exposes unavailable history as `NAE_AGAIN`.
   Transport tests cover delayed queries, discontinuities, expiration, zero
   frames and extreme timestamps. Player tests query during rendering.

6. **P2 — Failed edits mutate the player and wait for the renderer.** Add/remove/
   clear/timing/fader changes did not consistently roll back; positive `AGAIN`
   was treated as success. Commit polled for up to two seconds under the player
   lock. `429a4595b` stages replacement state, publishes once and rolls back on
   every non-OK result. Tests hold a commit pending, reject subsequent edits,
   then verify handles, duration and PCM after another successful commit.

7. **P2 — Export repeats preview loops.** Recording inherited the transport loop
   range and hold. `15a33aae6` disables preview looping for export and restores
   its settings afterwards. The sample-index fixture checks the full exported
   timeline with preview looping enabled.

8. **P2 — Frame and byte arithmetic overflow.** Timing endpoints, translated
   fader frames, initial decode position and WAV size calculations could overflow
   before validation. `426689f1b` checks arithmetic and rejects oversized WAV
   output before opening it. `21b67b166` subtracts the project offset before
   adding the media trim for loop endpoints. UBSan tests include `INT64_MAX`
   inputs and preservation of an existing output file on rejection.
   `b45d80799` also fixes Kotlin duration conversions and rejects infinity;
   its Android tests cover large durations.

9. **P2 — Short loops remain silent.** A range shorter than the declick ramp
   repeatedly restarted fades without producing useful audio. `13d9f482c`
   limits ramps to half the range. Tests cover 1, 2, 120 and 240 frames with a
   configured 240-frame ramp.

10. **P2 — Queue alignment exceeds the allocator guarantee.** `alignas(64)`
    inside a `calloc()`-allocated media object imposed an unsupported allocation
    requirement. `aefb1048f` separates the counters with padding at ordinary
    alignment. It also checks capacity arithmetic and lock-free atomics. Queue
    tests cover full/empty transitions, wrapping counters and capacity overflow.

Additional changes:

- `7125884ce`: run player tests on POSIX hosts, with Linux-only resource-usage
  checks guarded by `RUSAGE_THREAD`.
- `a80929c8c`: enable ngpu's compiler warnings and resolve conversion warnings.
- `9a8a0fe5e`: expand compact helpers, loops and cleanup statements; whitespace
  only.
- `b9f778b2d`: reject invalid Kotlin master volume assignments instead of silently
  ignoring the native error. Android test compiles; device execution is pending.
- `e42edea1b`: replace track capacity growth with fixed storage for the existing
  64-track limit.
- `6dc69946f`: shorten documentation and API comments; document four libraries,
  Android integration, edit retries, render ownership and clock-history limits.

## Remaining work

### Decoder scaling and callback deadlines

A paused host player with a local WAV used 6, 41 and 81 process threads with
1, 8 and 16 tracks respectively, from a one-thread baseline on macOS arm64.
Each track adds the NAE decoder plus nope.media control, demux, decode and
filter workers. Tracks outside the current playback range keep their contexts.
This measurement used Mach `task_threads()` after adding each track, without
starting host rendering; it is a thread-count measurement, not a CPU benchmark.

Bound the active decoder set around the playback window. Include pre-roll,
seeking, paused scrubbing, looping and shared files in the design. Pooling only
the NAE wrapper thread does not bound nope.media's workers. Measure CPU, RSS,
thread count, edit latency and callback deadline misses for representative Mojo
projects on Android and iOS before selecting limits.

The current tests check output and concurrency. Linux voluntary context-switch
counts do not prove callback deadlines, device latency or freedom from kernel
work. The macOS test does not collect those per-thread counts. Profile decoder
wakeups, queue traffic and real device callbacks under load.

### Edit costs and retirement

Each structural/timing/fader edit rebuilds the full graph, allocates plan
buffers and copies every reachable fader's keyframes. Opening a track also
starts its decoder before commit can return `AGAIN`; repeated retries can be
expensive. Measure burst edits with many tracks and keyframes. Consider batched
publication and immutable shared automation data with control-thread retirement.
Keep failed edits atomic and callbacks allocation-free.

Retired plans are collected by a later commit or explicit engine collection.
Check prompt release of removed decoders when no subsequent edit occurs; the
player has no explicit collection entry point. Add a resource-lifetime test
before changing that policy.

### Asynchronous media control failures

`run_decoder()` still counts later `nmd_seek()`, restart and loop-range failures
without publishing a terminal error for that request. Decide the recovery
contract, then propagate failure without presenting stale PCM as the requested
position. Add fault injection for seek, restart and loop updates, including an
export failure after some successful reads. The missing-file regression does
not cover these paths.

Loop start/end/revision are separate atomics. Review publication coherence
under rapid range changes; a race-free access is not necessarily a coherent
range snapshot. Existing concurrent PCM tests are useful but do not establish
that contract.

### API and platform integration

- Update Mojo callers for the breaking API changes below and run the Android
  instrumentation tests. The emulator available during review was offline.
- Validate physical Android and iOS output, interruptions, route changes,
  disconnection, stop/restart and video/audio synchronization. The sink estimates
  latency from buffering; there is no hardware presentation-position test.
- Test callback sizes and loop lengths that exhaust 256 clock segments. The API
  reports unavailable history rather than inventing a position; decide whether
  this bound meets Mojo's longest buffering and scheduling delays.
- Exercise allocation failures and thread/semaphore initialization failures.
  Existing tests do not cover all failure paths.
- Review the packed seek request limits before freezing the API: frame values
  use 48 bits and the seek serial uses 16. Include serial rollover with a stalled
  renderer in testing.
- Finish the style pass on positional initializers, combined declarations and
  remaining comments. Keep cosmetic changes separate from behavioral changes.

## API changes for callers

- Host mode: call `nae_player_start_rendering()` before launching the renderer.
  Join it before `nae_player_stop_rendering()` or destruction. `stop()` does not
  stop a host-owned thread.
- `nae_player_get_position_at(player, time, &position)` now returns a status.
  On `NAE_AGAIN`, the output is unchanged. The transport has the same history
  lookup contract. Kotlin `getPositionAt()` returns `Duration?`.
- Track edits can return `NAE_AGAIN` immediately. Retry outside the audio callback;
  retain the desired edit and the existing handle. `NAE_OK` is the only success
  status for edits.
- Kotlin `addTrack()` returns `TrackResult(status, track)`. A track is present
  only on `OK`. Removed/released handles throw; foreign handles are rejected.
- C track pointers are invalid after successful removal, clear or player
  destruction. Do not keep aliases to freed tracks. Kotlin checks do not change
  the C ownership contract.
- Kotlin time arguments must be finite. Invalid master volume assignments throw.
- Recording ignores preview loops and holds, restores their settings and leaves
  playback paused.

## Ring buffers and shared utilities

The media adapter uses the private C11 SPSC queue in `src/spsc.h`, with two
64-element queues: decoded block descriptors to the renderer and released
blocks back to the decoder. Samples remain owned by nope.media. The queue is
fixed-capacity and checks that its counters use lock-free atomics.

Pinned miniaudio 0.11.26 also contains `ma_rb` for bytes and `ma_pcm_rb` for PCM
frames. Both support one producer and one consumer. They are not used by the
media adapter; using them there would add a miniaudio dependency to the
nope.media adapter. `libnopegl/src/utils/job_queue.c` has circular storage with
mutexes and condition variables; it is not a replacement for this callback queue.

ngpu and nopegl have private `utils/darray` and checked-allocation helpers. The
player now uses a fixed array because its track count is capped at 64. A darray
would add machinery without removing a variable-size requirement there. For
future shared utilities, extract a small common layer or compile the existing
utility sources into a private target; do not link the audio core to the GPU
implementation just to obtain a container. Graph arrays are currently sized
once per compilation.

## Validation and reproduction

macOS arm64, Apple Clang 21, nope.media 14.0.0 and miniaudio 0.11.26:

- ASan/UBSan: all 8 tests pass with `UBSAN_OPTIONS=halt_on_error=1`.
- TSan: all 8 tests pass with `TSAN_OPTIONS=halt_on_error=1`.
- Additional ngpu warning flags: clean build.
- Android JNI: NDK 28.1 syntax checks pass for arm64 and armv7, API 29, with
  `-Wall -Wconversion`.
- Kotlin binding and instrumentation tests compile with Kotlin 2.3.21, JVM 17,
  Android API 36 and JUnit 4.13.2. Instrumentation tests were not executed.

With the pinned dependencies installed and their pkg-config directories in
`PKG_CONFIG_PATH`, use separate builds:

```sh
meson setup builddir/nae-asan libnae -Dbuildtype=debug \
    -Dnopemd=enabled -Dminiaudio=enabled -Db_sanitize=address,undefined
UBSAN_OPTIONS=halt_on_error=1 meson test -C builddir/nae-asan --print-errorlogs

meson setup builddir/nae-tsan libnae -Dbuildtype=debug \
    -Dnopemd=enabled -Dminiaudio=enabled -Db_sanitize=thread
TSAN_OPTIONS=halt_on_error=1 meson test -C builddir/nae-tsan --print-errorlogs
```

The local review builds are `/tmp/nae-review-build` and `/tmp/nae-review-tsan`.
Their pkg-config paths are `/tmp/nae-review-deps/lib/pkgconfig` and
`/Users/mbee/src/nope.gl/venv/lib/pkgconfig`. Test logs are in each build's
`meson-logs/testlog.txt`. The adapters and engine are instrumented; their
prebuilt third-party dependencies are not. Temporary reproducers are not needed
for the committed regression tests.

All fixes are local commits; nothing was pushed. Preserve unrelated untracked
IDE and workspace files when continuing this branch.
