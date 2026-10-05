# Shaders

In `nope.gl`, shaders are written in GLSL, but inputs and outputs are
abstracted by the tree. Similarly, `#version` directive and `precision` are
controlled outside the shader strings. This means that user-provided shaders
usually only contain functions.

Additionally, various helpers are provided to help writing portable shaders,
allowing node trees to be as portable as possible.

`Program` and `ComputeProgram` define GPU shader stages used by `Draw` and
`Compute`. `CustomPaint` and `Effect2DShader` instead provide color functions
that the engine integrates into a 2D draw. See [2D shaders](#2d-shaders) for
their entry points, coordinates and texture helpers.


## Vertex shader

### Builtin inputs

When constructing a graphic pipeline (`Draw` node), the geometry information
needs to be transmitted to the vertex stage. To achieve that, `nope.gl`
provides the following inputs to the vertex stage:

Type   | Name           | Description
-------|----------------|------------
`vec3` | `ngl_position` | geometry vertices, always available
`vec2` | `ngl_uvcoord`  | geometry uv coordinates, if provided by the geometry
`vec3` | `ngl_normal`   | geometry normals, if provided by the geometry
`int`  | `ngl_vertex_index`   | index of the current vertex
`int`  | `ngl_instance_index` | instance number of the current primitive in an instanced draw call

**Note**: these are commonly referred as `attributes` or `in` in GL lexicon.

### User inputs

To make accessible more vertex attributes, it is possible to add them in
`Draw.attributes` or `Draw.instance_attributes`. These attributes will be
set as inputs to the vertex shader. Since this parameter is a dictionary, the
user can access these inputs using the same name used as key parameter.

For example, given the following construct:

```python
center_buffer = ngl.BufferVec3(...)
color_buffer  = ngl.BufferVec4(...)
draw = ngl.Draw(geometry, program)
draw.update_attributes(center=center_buffer, color=color_buffer)
```

The vertex shader will get two extra attributes:

- `center` of type `vec3`
- `color` of type `vec4`

### Builtin variables

The variables are another type of inputs made accessible to the shader.
`nope.gl` provides the following as builtin:

Type   | Name                    | Stage    | Description
-------|-------------------------|----------|------------
`mat4` | `ngl_modelview_matrix`  | vertex   | modelview matrix
`mat4` | `ngl_projection_matrix` | vertex   | projection matrix
`mat3` | `ngl_normal_matrix`     | vertex   | normal matrix
`vec2` | `ngl_resolution`        | fragment | viewport size

**Note**: these are commonly referred as `uniforms` in GL lexicon.

The matrices are available in the vertex shader and follow the active camera
and transform nodes. `ngl_resolution` is available in the fragment shader and
contains the current viewport width and height.

A typical vertex shader will do the following computation to obtain the
appropriate vertex position: `ngl_projection_matrix * ngl_modelview_matrix *
vec4(ngl_position, 1.0)`.

Similarly, the normal vector is generally obtained using `ngl_normal_matrix *
ngl_normal`.

### User variables

To make accessible more vertex variables, it is possible to add them in
`Draw.vert_resources`. Since this parameter is a dictionary, the user can
access these inputs using the same name used as key parameter.

### Output position

To output the vertex position from the vertex shader, `ngl_out_pos` should be
used.

**Note**: this is commonly referred as `gl_Position` in GL lexicon.

### Outputs (to fragment)

To transmit information from the vertex stage to the fragment stage, the
outputs (vertex side) and inputs (fragment side) need to be declared as
`IO*` nodes in `Program.vert_out_vars`.

**Note**: these are commonly referred as `in`/`out` or `varying` in GL lexicon.


## Fragment shader

### Inputs

For a `Draw`, fragment inputs come from the vertex outputs declared using
`Program.vert_out_vars`. Uniforms include the builtin `ngl_resolution` and the
user resources described below. The fragment input passed to 2D paint and effect
functions is described in [2D shaders](#2d-shaders).

### User variables

To make accessible fragment variables, it is possible to add them in
`Draw.frag_resources`. Since this parameter is a dictionary, the user can
access these inputs using the same name used as key parameter.

### Output color

To output the color from the fragment shader, `ngl_out_color` should be used.

**Note**: this is commonly referred as `gl_FragColor` in GL lexicon.

In some cases (typically to render to a cube map), the user may want to output
more than one color. The `Program.nb_frag_output` parameter exists for this
purpose.


## Textures

Just like variables, textures are considered *resources* for the different
stages of the pipeline. This means they need to be added to
`Draw.vert_resources`, `Draw.frag_resources` or `Compute.resources` to be
respectively made accessible to the vertex stage, fragment stage or compute
stage.

Textures need special care in `nope.gl`, mainly because of the hardware
acceleration abstraction provided by the engine for multimedia files.

A `Texture2D` can have different types of source set to its `data_src`
parameter: it can be arbitrary user CPU data (typically `Buffer*` nodes) as
well as video frames (using the `Media` node).

To sample a `Texture2D` from within the shader, multiple options are available:

- `ngl_texvideo(name, coords)`: this picking method is safe for any type of
  `data_src`, but it is recommended to only use it with `Media`. The reason for
  this is that it adds a small overhead by checking the image layout in the
  shader: in the case of a media, that format can change dynamically due to
  various video decoding fall-back mechanisms. On the other hand, it is the
  only way to benefit from video decoding accelerations (external samplers on
  Android, VAAPI on Linux, etc).
- `texture(name, coords)`: this picking method should be used if and only if
  the `data_src` is *not* a `Media`. While it may work sometimes with a
  `Media`, it definitely won't if the video gets accelerated. The only safe way
  to use this picking method with a `Media` is to have
  `Texture2D.direct_rendering` disabled, but that may cause a costly
  intermediate conversion. If the `data_src` points to a user CPU node, then it
  is the recommended method.
- `imageLoad(name, icoords)`: if pixel accurate picking is needed, this method
  can be used. `icoords` needs to be integer coordinates. To use this method,
  it is required to indicate to the program that the texture needs to be
  exposed as an image. To achieve this, a `ResourceProps` node with
  `as_image=True` must be set to the program `properties`, using the name of
  the texture as key. Note that a texture can be accessed as a sampler from a
  program but as an image from another.

Texture metadata is available in both vertex and fragment stages for graphics
programs, and in the compute stage for compute programs. The texture sampler
itself is exposed in the stage where the resource is registered.

Availability | Type   | Name              | Description
-------------|-------|-------------------|------------
`Texture2D`, `Texture2DArray`, `Texture3D` |`mat4` | `%s_coord_matrix` | coordinate transform of the texture registered under key `%s`; for a 2D texture, apply it to `vec4(ngl_uvcoord, 0.0, 1.0)` and use `.xy` to obtain texture coordinates
`Texture2D`                                |`vec2` | `%s_dimensions`   | image width and height in pixels for the texture registered under key `%s`
`Texture2D`                                |`float`| `%s_ts`           | timestamp generated by the texture data source, 0.0 for images and buffers, frame timestamp for audios and videos


## Blocks

When using blocks as pipeline resources, their fields can be accessed within
the shaders using `<block>.<field-label>` where `<block>` is the key in the
resource nodes dictionary and `<field-label>` is the label of the field node.

For example, given the following construct:

```python
block = ngl.Block(fields=[
    ngl.UniformFloat(value=3.0, label="x"),
    ngl.BufferVec4(count=256, label="data"),
])
draw = ngl.Draw(geometry, program)
draw.update_frag_resources(blk=block)
```

A block with an instance name `blk` will be declared in the fragment shader, so
the first and second fields can respectively be accessed using `blk.x` and
`blk.data`.


## 2D shaders

`CustomPaint` computes the color of a `DrawRect2D` fill or a `Stroke2D` stroke.
The engine combines that color with shape coverage, antialiasing, clipping,
opacity and fill/stroke compositing. Your code supplies the color function within
that generated fragment shader.

`Effect2DShader` operates on the children of an `Effect2D`, rendered into an
offscreen texture. It can sample this input, change its colors or coordinates,
and combine it with other textures supplied as resources.

Both nodes accept one `glsl` string containing a complete entry function and
any helper functions it needs.

### Writing a paint

For a single output, define `main` with a fragment input and return an RGBA color.
This paint draws a checker pattern with cells 16 local pixels wide:

```glsl
vec4 main(const ngl_FragmentInput frag)
{
    vec2 rect_px = frag.rect_uv * frag.rect_size;
    float checker = mod(floor(rect_px.x / 16.0) + floor(rect_px.y / 16.0), 2.0);
    return vec4(vec3(checker), 1.0);
}
```

The parameter must be `const`; its name is your choice. The engine declares
`ngl_FragmentInput` and renames your entry before adding the GPU shader's own
`main`. It also declares resources and supplies the shader version and required
extensions. Omit those declarations and `#version` from your source. An empty
`CustomPaint.glsl` is invalid.

### Supplying resources

Use `resources` to supply uniforms, textures and blocks. Dictionary keys become
GLSL names. For example, this paint uses a uniform color for a horizontal fade:

```python
import pynopegl as ngl

paint = ngl.CustomPaint(
    glsl="""
        vec4 main(const ngl_FragmentInput frag) {
            float amount = clamp(frag.rect_uv.x, 0.0, 1.0);
            return vec4(tint * amount, 1.0);
        }
    """,
    resources={"tint": ngl.UniformVec3(value=(1.0, 0.5, 0.2))},
)
rect = ngl.DrawRect2D(rect=(20, 20, 240, 120), fill=paint)
canvas = ngl.Canvas2D(width=280, height=160, children=[rect])
```

The GLSL uses `tint` directly; do not add a `uniform vec3 tint` declaration.
Animated and evaluated values can also be supplied through resources.
Blocks use the same `resource_key.field_label` access described in
[Blocks](#blocks). `Effect2DShader.resources` follows the same rules.

### Fragment input and coordinates

Each public field is a `vec2`:

Field | Meaning
------|--------
`frag.rect_uv` | Position relative to the rectangle: `(0, 0)` at its origin and `(1, 1)` one rectangle width and height from that origin
`frag.rect_size` | Rectangle width and height in local pixel units, before geometric transforms
`frag.content_uv` | Rectangle UV after the content transform, before paint scaling and the texture coordinate transform
`frag.canvas_px` | Transformed position in the active canvas, in canvas pixel units

X points right and Y points down. Coordinates are not clamped: strokes,
antialiasing and transformed content can extend outside the rectangle. The
rectangle excludes the extra rasterization margin. A local pixel unit need not
be a physical framebuffer pixel after transforms or canvas density changes.

Fill content receives `DrawRect2D`'s content orientation, zoom and translation.
With paint scaling set to `"fit"`, zoom is ignored and translation is clamped to
keep the content within the rectangle. Stroke and effect content use
`content_uv == rect_uv`. The function
`ngl_content_uv(frag, p)` maps an arbitrary rectangle UV `p` to content UV and
agrees with `frag.content_uv` at `p == frag.rect_uv`.

The fragment input is a snapshot. Changing a field in a copy does not recompute
the other fields or the coordinates returned by the texture helpers. Pass
modified positions to the mapping helpers instead. Members prefixed with `_` and
the complete struct layout are implementation details.

### Texture helpers

For a `Texture2D` or a custom texture exposed as a 2D sampler, the resource key
names the sampler and its helpers. A texture registered as
`resources={"source": texture}` can be sampled with:

```glsl
vec4 main(const ngl_FragmentInput frag)
{
    return source_sample(frag);
}
```

This handles the texture's coordinate transform, including image flipping and
cropping, and uses `ngl_texvideo` for sampling images and video frames. The
helper preserves the sampled color's alpha representation; choose `premult`
according to [Alpha](#alpha).

There are three coordinate spaces involved in texture sampling:

- **Rectangle UVs** describe a position in the drawn rectangle.
- **Logical texture UVs** include the content transform and the paint's fit/fill
  scaling. They are suitable for shifting, rotating or distorting a sample.
- **Texture coordinates** also include the image's coordinate transform. They
  are suitable for passing directly to `ngl_texvideo` or a compatible sampler.

For the resource key `source`, the helpers are:

Function | Coordinate contract
---------|--------------------
`source_uv(frag)` | Current logical texture UV
`source_uv(frag, p)` | Map rectangle UV `p` to logical texture UV
`source_coord(frag)` | Current texture coordinates
`source_coord(frag, uv)` | Map logical texture UV `uv` to texture coordinates, applying the image's coordinate transform
`source_sample(frag)` | Sample at the current texture coordinates
`source_sample(frag, uv)` | Sample logical texture UV `uv`, applying the image's coordinate transform

`source_uv(frag, p)` accepts **rectangle UVs**; `source_coord(frag, uv)` and
`source_sample(frag, uv)` accept **logical texture UVs**. Passing `frag.rect_uv`
directly to them bypasses content and fit/fill mapping while retaining the
image's coordinate transform. `source_sample(frag, uv)` samples
`source_coord(frag, uv)` with `ngl_texvideo`: do not pass it the result of
`source_coord()`, that would apply the image's coordinate transform twice. A
coordinate belongs to its texture: each one includes its own texture's
coordinate transform, which can differ from another texture's, such as a render
target stored upside down. The `<key>_uv`, `<key>_coord` and `<key>_sample`
names are generated next to the `<key>_coord_matrix` and `<key>_dimensions`
metadata; do not define functions or variables with these names.
Helpers can be used from your own functions by passing the fragment input:

```glsl
vec4 rotated(const ngl_FragmentInput frag, float angle)
{
    vec2 uv = source_uv(frag);
    float c = cos(angle), s = sin(angle);
    uv = mat2(c, s, -s, c) * (uv - 0.5) + 0.5;
    return source_sample(frag, uv);
}

vec4 main(const ngl_FragmentInput frag)
{
    return rotated(frag, angle); // angle is a supplied uniform resource
}
```

To move the sample four local rectangle pixels down, map the rectangle position
before sampling:

```glsl
vec4 main(const ngl_FragmentInput frag)
{
    vec2 p = frag.rect_uv + vec2(0.0, 4.0) / frag.rect_size;
    return source_sample(frag, source_uv(frag, p));
}
```

`CustomPaint` uses the first eligible 2D texture in resource dictionary order as
its scaling reference, even if the shader does not sample that texture. Array,
volume and cube textures are skipped and use their usual sampling interfaces.
Other eligible textures share the paint's logical UV mapping, with a separate
coordinate transform for each image, hence the coordinates named after each
texture.

The helpers do not add clamping or transparent borders: texture sampler settings
apply. `TexturePaint.wrap="discard"` has a separate logical-image bounds check.
`CustomPaint` has no `wrap` parameter; implement any such bounds check in GLSL.

### Effects and their input

An `Effect2DShader` receives the same fragment input type. Its rectangle is the
full effect bounds, including dilation. Both its content UVs and logical texture
UVs equal `frag.rect_uv`.

The input is the texture `ngl_input`, with the same helpers as a texture
resource: `ngl_input_uv()`, `ngl_input_coord()` and `ngl_input_sample()`. This
entry returns the rendered children unchanged:

```glsl
vec4 main(const ngl_FragmentInput frag)
{
    return ngl_input_sample(frag);
}
```

An empty `Effect2DShader.glsl` also passes the children through. Effect and parent
opacity still apply. `Effect2D.shaders` selects the first shader whose time range
is active. To apply successive effects, nest `Effect2D` nodes.

For explicit sampling, `ngl_input_uv(frag, p)` equals `p`. `frag.rect_size`
describes the effect bounds in local units; `ngl_input_dimensions` describes the
allocated texture in pixels. They can differ, so do not use texture dimensions as
the effect's rectangle size.

Extra textures in `Effect2DShader.resources` have their own image coordinate
transforms. They use rectangle UVs as their logical UVs and do not inherit the
effect input's transform or a paint's fit/fill scaling.

`Layer2D` and `Mask2D` are effects without shaders: their children are rendered
into the effect input, which is composited as is, or weighted by a channel of
the mask. A mask texture covers the effect rectangle: the children bounds, their
anti-aliased edges included, or `Mask2D.mask_rect`. Mask children are drawn in
the local space of the children and on the same texels as the input, so a mask
shape lines up with the content it masks whatever the bounds or the output
resolution. Both hold even where the effect extends beyond the canvas.

### Alpha

The 2D compositor uses premultiplied colors: RGB is multiplied by alpha. For
example, red at 50% opacity is `(0.5, 0.0, 0.0, 0.5)` in this representation,
compared with the straight-alpha color `(1.0, 0.0, 0.0, 0.5)`.

`premult=True` asks the engine to multiply your returned RGB by alpha before
compositing. Use it when your function returns a straight-alpha color.
`premult=False` leaves the returned color unchanged, so your function must return
premultiplied RGB.

Node | Default `premult` | Expected return value with that default
-----|-------------------|----------------------------------------
`CustomPaint` | `True` | Straight-alpha color
`Effect2DShader` | `False` | Premultiplied color

The effect input already contains premultiplied colors, so the passthrough
example uses the effect's default `premult=False`. Likewise, set
`CustomPaint.premult=False` when returning a sample from an `OffscreenCanvas2D`
color texture. Multiplying such a color by its alpha again darkens translucent
pixels. The texture helpers add no alpha conversion of their own.

### Multiple outputs

With `CustomPaint.color_output_count > 0`, define a `void` entry and write each
output directly. For `color_output_count=2`:

```glsl
void main(const ngl_FragmentInput frag)
{
    ngl_out_color[0] = vec4(frag.rect_uv.x, 0.0, 0.0, 1.0);
    ngl_out_color[1] = vec4(0.0, 0.0, frag.rect_uv.y, 1.0);
}
```

This mode bypasses the usual paint coverage, fragment clipping, opacity and
premultiplication code. It cannot be used as a stroke or combined with one.
`Effect2DShader` always uses the single-color return signature.

### Source and resource rules

Resource keys must be GLSL identifiers of at most 63 characters, must not start
with `gl_`, `ngl_` or `ngli_`, and must not equal `main`. Coordinate names such
as `rect_uv`, `content_uv` and `canvas_px` are allowed as keys because coordinates
are fragment input members.

`main` is reserved for the literal entry definition and its optional matching
prototype. Do not generate it with a macro or use it as a variable, field or
type. Entry rewriting leaves comments and preprocessor directives unchanged. The
GLSL compiler handles macros, conditionals and source validation. Diagnostics
identify user source lines through `#line`. Source IDs are 1 for fill, 2 for
stroke and 3 for an effect; for example, a diagnostic at source 3, line 17 refers
to line 17 of the effect's `glsl` string.

Fill and stroke still share a namespace and preprocessor environment. A resource
key present in both must bind the same node. Identical custom paint sources are
emitted once and receive a separate fragment input for each role. Distinct
sources should use distinct helper names or explicitly guard identical shared
declarations:

```glsl
#ifndef SHARED_PAINT_SHADE
#define SHARED_PAINT_SHADE
vec4 shade(vec4 color, float amount)
{
    return vec4(color.rgb * amount, color.a);
}
#endif
```

Texture helpers and coordinates are generated for every eligible bound texture,
including textures that the source does not sample. Shared functions and macros
receive valid texture coordinates for both roles. A draw whose explicit varying
locations exceed backend limits is rejected. Default sampling reads interpolated
texture coordinates directly; only explicit mapping and sampling overloads perform
coordinate arithmetic per fragment.
