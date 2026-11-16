# nope.audio code style and error handling handoff

This document records a source review comparing `libnopeaudio` with
`libnopegl` and `libnopegpu`, followed by the status of the resulting changes.
The original review did not change implementation code or run tests. The
findings below describe that historical baseline; the current status records
the subsequent implementation and API review.

## Baseline and scope

- Review date: 2026-11-16.
- Repository: `/home/mateo/src/nope.gl`.
- Reviewed HEAD: `ec1cd7788` (`nopeaudio: add a sidechain ducker`).
- Recheck the live checkout before implementing further changes and preserve
  unrelated user work, including untracked files.
- The original review concerned error handling, implementation conventions
  and formatting. The subsequent user-directed review also changed APIs and
  folded the changes into the existing commit series.

## Current status and conventions

- Player and media constructors now check mutex initialization before
  publishing an object. Player edits assign errors at the failure branch,
  use immediate cleanup exits, and retain rollback on `NAU_AGAIN`.
- Guaranteed media queue pushes and player rollback invariants now use
  `nau_assert`. Processor and callback initializers use aligned fields.
- Nodes separate allocation (`nau_node_create()`) from initialization
  (`nau_node_init()` and the specialized `*_init()` functions). Failed
  initialization leaves the allocation available for retry or
  `nau_node_unrefp()`.
- WAV output uses the generic FFmpeg exporter. Both components separate
  `*_create()` from `*_open()` and clear the caller's pointer with
  `*_closep()`. Exporter format and codec options allow other containers
  and codecs without changing the rendering path.
- Item counts use `count` or singular `*_count`, and callback tables use
  `_callbacks`. Keep dependency-defined field names unchanged.
- A `render_plan` is the committed processing order, buffers and snapshots
  for a graph. Each `render_step` describes one node's processing call and
  its input/output buffers. Snapshots freeze editable settings for that
  plan while retaining the node's rendering state. See the graph and
  ownership comments in [engine.c](src/engine.c).
- Shared math and time constants live in the utility headers. Use uppercase
  `U` suffixes and do not add `(void)param` casts for unused parameters;
  that warning is disabled for this library.
- Contextual dependency diagnostics remain a possible follow-up; no common
  logging interface was added.

Validation after the refactor: 17 SDL3 tests, 11 core/null tests and 16
ASan/UBSan tests passed. The JNI syntax check and the combined underwater
filter/ducker example also passed. Android runtime tests and TSan were not
run as part of this validation.

The overall C conventions already match: allocation wrappers, `sizeof(*ptr)`,
negative errors, cleanup labels, `*_freep()` helpers, static functions and
callback classes. Keep these patterns.

The [developer guidelines](../doc/dev/ref/developers.md) ask contributors to
follow the surrounding code, check errors, assert impossible paths and keep
cosmetic fixes in dedicated commits. The two graphics libraries are not
perfectly uniform; the examples below establish useful patterns, not a
claim that every existing file follows them.

## Priority 1: handle mutex initialization failures

This is a correctness fix and should be separate from cosmetic changes.

In the reviewed baseline, [player.c](src/player.c)'s `nau_player_create()`
ignored the return from `pthread_mutex_init(&p->lock, NULL)`. In
[source_nopemedia.c](src/source/source_nopemedia.c), `nau_media_create()`
did the same for `m->loop_lock`. Both constructors could continue with an
uninitialized mutex, which later operations used and destruction attempted
to destroy. Both initialization failures are now handled.

Use `vk_create()` in
[libnopegpu's ctx_vk.c](../libnopegpu/src/vulkan/ctx_vk.c) as a reference:
check mutex initialization immediately, release the allocation on failure,
and return failure before publishing the object.

Implementation requirements:

- Pthread functions return zero on success and a positive error number on
  failure. Check `!= 0`, not `< 0`.
- Translate failure into an appropriate existing negative `NAU_ERROR_*`
  value; do not return a positive pthread error as an audio status.
- At these two initialization points, only the outer object has been
  allocated. Directly freeing it is sufficient. Do not call a general
  destructor that assumes the mutex or later atomic fields are initialized.
- Keep output pointers NULL on failure and preserve normal ownership after
  successful initialization.
- Do not introduce an initialization flag unless the resulting cleanup
  structure actually needs one.

## Priority 2: make failure paths explicit and local

### Assign an error inside the branch that detects it

The reviewed `nau_player_add_track_effect()` preassigned `NAU_ERROR_INVALID`,
then `NAU_ERROR_UNSUPPORTED`, then `NAU_ERROR_MEMORY` before the operations
that could fail. Several other player functions and `nau_media_create()` used
the same pattern. These assignments now accompany their failure branches.

Prefer the pattern used by `ngli_ctx_configure()` in
[libnopegl's api.c](../libnopegl/src/api.c):

```c
e = nau_try_calloc(1, sizeof(*e));
if (!e) {
    ret = NAU_ERROR_MEMORY;
    goto end;
}
```

This keeps the error reason next to its condition and avoids depending on
an assignment several operations earlier. Preserve existing error values
unless a separately justified correctness fix requires a change.

### Prefer sequential operations with immediate failure exits

The reviewed `nau_player_set_track_fader()` chained construction, ramp
assignment and commit through repeated `if (ret == NAU_OK)` checks. Similar
patterns appeared in timing and automation edits. These paths now use
sequential operations with immediate failure exits.

Where this improves readability, use a straight sequence of operations with
immediate exits to the appropriate cleanup path, as commonly done in the
graphics libraries. Keep simple success checks that directly express an
operation's outcome; this is not a blanket ban on `if (ret == NAU_OK)`.

Preserve complete transactions:

- Release temporary nodes and arrays on failure.
- Unlock the player mutex exactly once on every applicable path.
- Publish the edited track only after a successful commit.
- Restore source ranges, origins, automation and sidechain keys where the
  existing edit requires rollback.
- Preserve stateful effect node identity.
- Never jump to cleanup that reads an uninitialized local variable.

### Keep the distinction between errors and positive statuses

The [public header](src/include/nopeaudio/nopeaudio.h.in) defines:

| Return | Meaning |
| --- | --- |
| `NAU_OK` / 0 | Success |
| `NAU_AGAIN` / 1 | Temporarily unavailable; retry later |
| `NAU_EOF` / 2 | End of a source |
| Negative `NAU_ERROR_*` | Error |

A commit returning `NAU_AGAIN` must stop and roll back the edit. For that
operation, preserve `ret != NAU_OK` or an equivalent explicit check. Do not
mechanically replace audio checks with the graphics libraries' common
`ret < 0` idiom. Choose each condition from the called function's contract.
Likewise, EOF and temporary starvation have distinct rendering semantics.

## Priority 3: assert important internal invariants

Audio defines `nau_assert` in [utils.h](src/utils/utils.h), but the reviewed
implementation files contained no calls to it. Both graphics libraries use
their corresponding helpers extensively. See `remove_scene_node()` in
[libnopegl's scene.c](../libnopegl/src/scene.c) for an example of checking an
internal ownership/index invariant.

In the reviewed media source, `push_block()` and `give_back()` ignored
`spsc_push()`'s result because capacity accounting guarantees success. The
implementation now preserves that explanation and checks the invariant:

```c
const bool pushed = spsc_push(&m->blocks, block);
nau_assert(pushed);
```

Use the same reasoning for any other guaranteed queue pushes, including
shutdown, after checking their ownership and capacity assumptions. Add only
assertions for established invariants, not speculative constraints.

- Public invalid input and expected runtime failures must still return
  errors or statuses; do not replace their validation with assertions.
- Evaluate operations with side effects separately from assertions.
- Use the existing `nau_assert` helper. Its current implementation is active
  in release builds and aborts on an invariant failure.
- A successful render path must still avoid logging, allocation, freeing,
  locks and waits. An assertion is for an unrecoverable programming error,
  not an underrun or a recoverable full queue.

## Priority 4: normalize formatting

Keep formatting changes separate from changes to control flow or behavior.
Use neighboring graphics code as the reference rather than applying a
formatter indiscriminately to the entire library.

The audio code frequently packs several initializer fields or declarations
onto one line. For example, the processor descriptor in
[node_gain.c](src/node/node_gain.c) should read like:

```c
const struct nau_processor processor = {
    .input_count = 1,
    .user        = g,
    .process     = gain_process,
    .reset       = gain_reset,
    .destroy     = nau_free,
};
```

Apply the same treatment where useful to callback descriptors and argument
structures, such as `chunk_block` in [effect.c](src/effect/effect.c).

Other cleanup:

- Give distinct struct members their own declarations, for example
  `ramp_frames` and `remaining`, or `target` and `current` in `struct gain`.
- Align multiline arguments consistently with nearby code.
- Add blank lines between validation, allocation, initialization and
  publication when these are distinct steps.
- Preserve concise mathematical tables and small initializers where their
  existing presentation makes comparison easier.
- Preserve comments explaining ownership, publication, rollback and thread
  constraints; these are valuable even when surrounding code is reformatted.
- Do not rewrite copied utility/platform code merely to satisfy a newly
  invented local style.

## Priority 5: improve failure diagnostics outside the render callback

The graphics libraries commonly provide contextual diagnostics alongside
errors. Audio has SDL and Oboe diagnostics, but its media initialization
maps multiple dependency failures to `NAU_ERROR_IO` without adding the
failing operation's context at that layer.

Consider a small, separate follow-up that identifies failures during decoder
option setup, seeking, starting and information retrieval. The original
dependency result should be available in diagnostics where practical;
public error translation can remain unchanged.

This needs an explicit implementation choice because audio has no common
logging interface equivalent to the graphics libraries' `LOG` helpers.
Keep any private logging support small and independently reviewable. Do not
couple core/media code to SDL merely to obtain logging, add a public logging
API as an incidental cleanup, or assume all dependency failures lack logs
of their own. Avoid duplicate diagnostics where the lower layer already
provides enough context.

Initialization/control and decoding-thread diagnostics are distinct from
real-time callbacks. Keep normal logging off the real-time render path;
continue using the existing status/statistics mechanisms for runtime events.

## Design differences to preserve during this work

These are API choices, not mechanical style fixes:

- Nodes and output writers now separate pointer-returning `create()` from
  status-returning `init()` or `open()`. Keep these lifecycle stages explicit
  and release allocations after failed initialization. Other existing audio
  constructors still return a status and set an output pointer.
- Audio has fewer error categories and positive streaming statuses. Do not
  rename errors, change their values or adopt the graphics error encoding as
  part of this pass.
- Keep audio's real-time constraints, explicit atomic publication and
  transactional edits. Do not copy graphics runtime behavior without checking
  whether it is appropriate on an audio callback.
- Keep the current node/effect abstraction. Adding a generic class framework
  is not required for style consistency.

## Validation and delivery

Implement in priority order, with correctness, error-path cleanup, invariant
checks and pure formatting kept independently reviewable. Diagnostics can
follow as its own change. Do not rewrite the existing commit series unless
the user separately requests it.

Use the [existing test instructions](README.md) and configured dependencies.
Relevant coverage includes player edit rollback and concurrent edits,
media decoding and concurrent reads, SPSC queues, engine/effects, transport,
export and sink mocks. Keep the existing `NAU_AGAIN` rollback and real-time
assertions intact.

For mutex initialization failure, use a focused failure-injection test if
the existing harness can support it without invasive production hooks. At
minimum, trace and review the early cleanup explicitly; normal successful
construction tests do not exercise that failure path.

Run affected tests after logical changes, then validate the final state with
media/player enabled and with `-Dnopemedia=disabled -Dsink=null`. Include
separate ASan/UBSan and TSan builds for ownership and concurrency changes
where supported. Pure formatting does not justify new behavioral tests.
Investigate warnings and failures rather than weakening checks.

Check `git diff --check`, verify intended public API changes have matching
documentation and caller updates, verify unrelated files are unchanged,
and report:

- Which findings were addressed and any intentionally deferred work.
- How error statuses, rollback and real-time behavior were preserved.
- Builds/tests actually run, their results and unavailable configurations.
- Any remaining design decision around diagnostics.

Do not claim this document's source review as runtime validation of the
implementation changes.
