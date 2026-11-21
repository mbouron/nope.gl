# Working on nope.gl

Write for the human who will learn, review, and maintain this project. Code,
comments, and documentation should be clear, precise, and durable. Someone
returning years later should be able to understand why the code works and
change it safely without access to the original conversation.

Keep related steps together so a reader can follow one decision at a time.
If understanding a condition requires tracing several flags through a chain
of helpers, simplify that path. Name the distinction that matters, such as
scene time versus media time, and use those terms consistently in the code,
comments, tests, and docs.

## Explaining code with diagrams

Choose the diagram that answers the reader's question. A call tree shows
who calls whom. A data-flow diagram can show how a frame moves from the
decoder to the renderer. A timeline can show why a frame plays too early.
Prefer ASCII in comments, commit messages, and chat so the diagram is readable
as plain text. In rendered docs, Mermaid is also an option, as in the
[architecture overview](doc/dev/ref/architecture.md).

### Showing who calls whom

Start at the caller the reader needs to understand. Indent each callee under
its caller, in execution order. Use names the reader can find in the source.
Here the calls from `draw()` show when nodes get their resources and when
they use them.

```text
draw()
|-- prepare_draw()
|   `-- update_scene()
|       |-- ngli_node_honor_release_prefetch()
|       |   |-- ngli_node_visit()   collect activity across branches
|       |   |-- node_release()      parents before children
|       |   `-- node_prefetch()     children before parents
|       `-- ngli_node_update()
|-- ngli_node_pre_draw()
`-- ngli_node_draw()
```

Two branches can share a node. Visiting both before releasing resources
keeps an inactive branch from releasing a node the active branch still needs.

### Showing timing

For a race, use one column per thread and align the events that can overlap.
For a timing change, put the old and new behavior on the same time axis.

For example, seeking before the first audio frame used to play it too early.
Here `|` marks the seek target and `[A]` is the first audio frame.

```text
media time     ----|-----[A]----
before fix     ----[A]----------
after fix      ----|-----[A]----
```

Keeping the frame's original timestamp lets the caller preserve the gap.

## Code and comments

- Follow the file you are editing. In `libnopegl`, for example, public C
  functions use `ngl_`, internal functions shared across files use `ngli_`, and
  file-local helpers are `static`. For Python, follow the Black and isort
  settings in [pyproject.toml](pyproject.toml).
- Keep ownership and error handling visible. When a C call can return a
  negative error code, check it and preserve the reason for failure. Follow
  the cleanup path for resources the function already owns.
- Comment where a reader needs the reason for a decision. For example,
  changing a time range invalidates its children's cached timestamps because
  the next draw can request the same scene time.
- When changing an interface, check its callers, bindings, tests, and docs.
  Keep public enum values and function signatures stable unless the change
  needs to break them. The [exposure rules](doc/dev/ref/developers.md#exposure)
  say when a break requires a major version bump. After changing node
  parameters, regenerate `libnopegl/nodes.specs` with `make nopegl-updatespecs`.

## Commits and review

- Each commit must be atomic. Keep one logical change together with the
  tests and documentation it needs. Each commit should build, pass its
  tests, and make sense on its own. Put unrelated formatting changes in a
  separate commit. Atomic does not mean minimal: edits that serve one goal,
  such as several additions to the same checklist, belong in one commit.
  Split only when a reviewer would judge the parts separately.
- Check `git log --oneline -- <path>` for the prefix used in that part of
  the tree. Use a specific, imperative subject, such as
  `filterschain: reject truncated resource names`.
- Write the commit body for a reviewer who first encounters the problem here.
  Say what the diff cannot show, such as what triggers the problem, what the
  caller observes, and why the change works; do not restate the diff. Keep
  the body in proportion to the change: a small fix usually needs a sentence
  or two, not the investigation that led to it. A fix whose subject says it
  all needs no body. Include reproduction steps when the reviewer could not
  see the behavior otherwise.
- Choose a test that observes the changed behavior. For a rendering change,
  inspect the image diff before updating a reference. On a configured tree,
  `make tests` runs the suite. The [testing guide](doc/dev/howto/tests.md)
  shows how to run a single test or backend.
- In a code review, point to the line responsible, give the input or sequence
  of events that triggers the problem, and describe the observable effect.
  If several symptoms share a cause, explain them in one finding.
- Before proposing to split, reword, or rewrite existing commits, ask what
  someone reading `git log` later would gain. If the answer is nothing,
  leave the history alone.

See the [developer guidelines](doc/dev/ref/developers.md) for the full
review checklist.
