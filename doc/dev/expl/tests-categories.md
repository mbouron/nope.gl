# Tests categories

## Simple tests

In the tests suite, you will find a few straightforward tests written as simple
Python functions, calling `assert` directly. These tests usually cover
top-level API usage or anything non-visual. Some examples can be found in
`tests/api.py`.

`libnopegl` also includes dedicated programs for unit-tests (see
`libnopegl/src/test_*`) to cover code which is usually harder to access from a
top-level interface like the one provided by the Python binding.

## Floats

Some tests will compare series of float values. This is typically the case for
the animations/easings.

These tests are using the `@test_floats` decorator.

## Render

Render based tests perform a pixel-level comparison between the rendered output
and PNG reference images. They operate at full resolution and detect per-pixel
differences with anti-aliasing awareness: pixels that differ due to
anti-aliasing are identified and forgiven, so only real differences are counted
as failures.

These tests are using the `@test_render` decorator.

A few options are available such as the export resolution, per-channel
`tolerance` (how much each color channel can differ before a pixel is
considered different) and `diff_threshold` (the fraction of truly different
pixels allowed before the test fails).

When tests are run, an HTML report is generated listing all results. Tests with
differences include side-by-side, diff, flip and swipe views for visual
inspection.
