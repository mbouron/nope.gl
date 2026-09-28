# 2D coordinates

The 2D nodes (`Canvas2D`, `DrawRect2D`, `Effect2D`…) share one convention for
every coordinate they expose, and every shader they run receives the same set of
coordinates.

## Convention

- The origin is the **top-left** corner and **Y points down**, in canvas units:
  a `Canvas2D` of `width`×`height` spans `[0, width]`×`[0, height]`.
- Coordinates follow the **pixel-edge** convention, like Skia or ThorVG: the
  pixel `(i, j)` covers `[i, i+1]`×`[j, j+1]`, so its center is at
  `(i + 0.5, j + 0.5)`.
- Texture coordinates are **image coordinates**: `(0, 0)` is the top-left corner
  of the image as it is meant to be seen, whatever its layout in memory (render
  targets stored upside down, rotated or cropped video frames…). Sample them
  with `ngl_teximage()` (see [textures][shadertex]).

[shadertex]: shaders.md#textures

## Shader inputs

`CustomPaint.glsl_color` and `Effect2DShader.glsl_color` are the body of a
function receiving the following `vec2` parameters:

Name         | `DrawRect2D` fill                                   | `DrawRect2D` stroke             | `Effect2D`
-------------|-----------------------------------------------------|---------------------------------|------------------------------------------
`rect_uv`    | position in the rect, `[0, 1]`                      | same as the fill                | position in the effect rect, `[0, 1]`
`rect_px`    | `rect_uv` in canvas units from the rect corner      | same as the fill                | `rect_uv` in canvas units from the effect rect corner
`content_uv` | `rect_uv` after the content transform               | `rect_uv`                       | `rect_uv`
`tex_coord`  | image coordinates of the paint texture              | image coordinates of the stroke paint texture | image coordinates of `ngl_input`
`canvas_px`  | position on the canvas                              | same as the fill                | position on the canvas

- `rect_uv` goes slightly outside `[0, 1]` for the fragments drawing the
  anti-aliased edges and the outside of the strokes.
- The **content transform** of a `DrawRect2D` only applies to its fill: it
  rotates (`content_orientation`), zooms (`content_zoom`) and translates
  (`content_translate`) around the rect center:
  `content_uv = R · ((rect_uv - 0.5) / content_zoom + content_translate) + 0.5`.
- `tex_coord` is `content_uv` (or `rect_uv` for a stroke) scaled around the
  center to fit or fill the texture as the paint `scaling` says. The texture of a
  `CustomPaint` is its first texture resource; without a texture, `tex_coord`
  equals `content_uv`.
- An effect is rendered from the canvas region it covers: its rect is the
  bounding box of its children (or the rect given by `bounds`) extended by
  its `dilation`. `rect_uv` spans that whole rect even where it is cropped to
  the visible canvas, while `ngl_input` only holds the visible part: sample it
  at `tex_coord`.

## Sampling elsewhere than at the fragment

`tex_coord` is the texture coordinate of the fragment being drawn. A shader
sampling its texture at another position of the rect (a distortion, a
transition…) gets the matching texture coordinate with
`ngl_tex_coord(rect_uv)`: it applies the same scaling and content transform as
`tex_coord`, so `ngl_tex_coord(rect_uv) == tex_coord`. In an `Effect2D`, it maps
to the image coordinates of `ngl_input`, which only covers the visible part of
a cropped effect:

```glsl
vec2 p = rect_uv + vec2(0.02 * sin(rect_uv.y * 40.0), 0.0);
return ngl_teximage(ngl_input, ngl_tex_coord(p));
```

A `glsl_header` shared by the fill and the stroke paints cannot call
`ngl_tex_coord()`, since it differs for each of them.

## Paint resources

The resources of a `CustomPaint` are referenced in its GLSL by the key they are
bound to, and its GLSL is inserted as is. The fill and the stroke paints of a
`DrawRect2D` are compiled in the same shader, so:

- a paint cannot be both the fill and the stroke of a draw;
- the fill and stroke paints cannot bind the same resource key;
- they cannot declare the same symbols in their `glsl_header`, unless they use
  the very same header, which is then declared once.

The resources of the built-in paints (`ColorPaint`, `TexturePaint`…) never
clash with anything.
