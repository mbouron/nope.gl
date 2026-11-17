# Project architecture and organization

The `nope.gl` project is split in several parts:

- `libnopegl`: the scene graph engine.
- `libnopegpu`: the GPU abstraction layer (OpenGL, OpenGL ES and Vulkan).
- `libnopemedia`: the media playback library licensed under the LGPL 2.1 or
  later (formerly the `nope.media` project).
- `ngl-tools`: a set of various program tools using `libnopegl`, including a
  viewer (`ngl-viewer`) to display and export scenes.
- `pynopegl`: a Python binding for `libnopegl` (with the help of `Cython`) to
  create graph scenes in the most simple way.
- `pynopegl-utils`: Python utilities for exporting and testing scenes, with
  rendering examples.

## Dependencies

- `libnopegl` requires a standard C toolchain and [meson build system][meson].
  It uses `libnopegpu` for rendering and `libnopemedia` for media playback.
  [Graphviz][graphviz] is optional but can be used to render and preview graphs
  obtained from the API.
- `libnopemedia` requires a standard C toolchain, [meson build system][meson] and
  [FFmpeg][ffmpeg].
- `ngl-tools` needs [SDL3][sdl3] and `libnopegl` installed.
- `pynopegl` needs [Python][python] and [Cython][cython], and `libnopegl`
  installed.
- `pynopegl-utils` needs [Python][python] and `pynopegl`. The test tools
  depend on `NumPy` and `Jinja`.

```{mermaid}
graph
    libnopegl(libnopegl)
    libnopegpu(libnopegpu)
    libnopemedia(libnopemedia)
    ngl_tools(ngl-tools)
    pynopegl(pynopegl)
    pynopegl_utils(pynopegl-utils)

    cython[Cython]
    ffmpeg[FFmpeg]
    python[Python]
    sdl3[SDL3]
    numpy[NumPy]
    jinja[Jinja]

    libnopemedia --> ffmpeg
    libnopegl --> libnopegpu
    libnopegl --> libnopemedia
    ngl_tools --> sdl3
    ngl_tools --> libnopegl
    ngl_tools -. ngl-python .-> python
    pynopegl --> libnopegl
    pynopegl --> python
    pynopegl --> cython
    pynopegl_utils --> pynopegl
    pynopegl_utils --> python
    pynopegl_utils -.-> graphviz
    pynopegl_utils --> numpy
    pynopegl_utils --> jinja

    classDef ngldep fill:#add8e6,color:#222
    classDef extdep fill:#c0c0c0,color:#222
    class libnopegl,libnopegpu,libnopemedia,ngl_tools,pynopegl,pynopegl_utils ngldep
    class ffmpeg,sdl3,python,cython,graphviz,numpy,jinja extdep
```

[meson]: https://mesonbuild.com/
[ffmpeg]: https://ffmpeg.org/
[graphviz]: http://www.graphviz.org/
[python]: https://www.python.org/
[cython]: http://cython.org/
[sdl3]: https://www.libsdl.org/
