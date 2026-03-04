project('ngl-tests', meson_version: '>= 0.56.0')

ngl_test = find_program('ngl-test')
ngl_probe = find_program('ngl-probe')

backends = []
probe_output = run_command(ngl_probe, check: true).stdout()
message('ngl-probe:\n' + probe_output)
foreach line : probe_output.split('\n')
  if line.startswith('- ')
    backends += line.substring(2, -1)
  endif
endforeach

foreach backend : backends
  cap_cmd = [ngl_probe, '-l', 'quiet', '-b', backend, '--cap']
  # ngl-probe may display garbage on stdout: typically Mesa does this on
  # offscreen at initialization, this is why we have .split()[-1] to read the
  # last word in the output.
  has_compute            = run_command(cap_cmd + ['compute'], check: true).stdout().split()[-1].to_int() == 1 ? true : false
  has_ds_resolve         = run_command(cap_cmd + ['depth_stencil_resolve'], check: true).stdout().split()[-1].to_int() == 1 ? true : false
  max_color_attachments  = run_command(cap_cmd + ['max_color_attachments'], check: true).stdout().split()[-1].to_int()
  max_samples            = run_command(cap_cmd + ['max_samples'], check: true).stdout().split()[-1].to_int()
  has_text_libraries     = run_command(cap_cmd + ['text_libraries'], check: true).stdout().split()[-1].to_int() == 1 ? true : false

  message('Backend: ' + backend)
  message('- Compute: @0@'.format(has_compute))
  message('- Max color attachments: @0@'.format(max_color_attachments))
  message('- Max samples: @0@'.format(max_samples))

  tests_drawrect = [
    'color_fill',
    'gradient_fill',
    'gradient_fill_diagonal',
    'gradient4_fill',
    'noise_fill',
    'stroke_inside',
    'stroke_center',
    'stroke_outside',
    'stroke_dashed',
    'stroke_dashed_round',
    'stroke_dashed_square',
    'stroke_gradient',
    'stroke_gradient4',
    'corner_radius',
    'corner_radius_stroke',
    'opacity',
    'clip_rect',
    'content_zoom',
    'content_translate',
    'translate',
    'scale',
    'rotate',
    'animated_opacity',
    'group',
    'blending_src_over',
    'texture_fill_none',
    'texture_fill_fit',
    'texture_fill_fill',
    'texture_fill_wrap_discard',
    'texture_fill_fit_translate',
    'texture_fill_fill_zoom',
    'custom_checkerboard',
    'custom_radial_gradient',
    'custom_wave',
    'custom_vignette',
    'custom_texture',
    'custom_block',
  ]

  env = ['BACKEND=' + backend]

  foreach test_name : tests_drawrect
    func_name = 'drawrect_@0@'.format(test_name)
    # Pass the ref base path (no extension); the PNG comparator appends
    # ".png" for single-frame tests or "_N.png" for multi-frame tests.
    ref_base = meson.current_source_dir() / 'refs/@0@'.format(func_name)
    test(
      test_name,
      ngl_test,
      args: [files('drawrect.py'), func_name, ref_base],
      env: env,
      suite: [backend, 'drawrect'],
    )
  endforeach

  # compute tests (Compute + DrawRect + test_png, requires compute capability)
  if has_compute
    tests_compute = [
      'histogram',
      'image_load_store',
      'image_3d_load_store',
      'image_2d_array_load_store',
      'image_cube_load_store',
    ]

    foreach test_name : tests_compute
      func_name = 'compute_@0@'.format(test_name)
      ref_base = meson.current_source_dir() / 'refs/@0@'.format(func_name)
      test(
        test_name,
        ngl_test,
        args: [files('compute.py'), func_name, ref_base],
        env: env,
        suite: [backend, 'compute'],
      )
    endforeach
  endif

  # effect tests (DrawRect + CustomFill + test_png)
  tests_effects = [
    'bokeh',
    'concreteframe',
    'crosses',
    'crosses2',
    'crt',
    'dustframe',
    'dustscratches',
    'dustscratchessoft',
    'dustscratchesyellow',
    'eightmmframe',
    'ember',
    'filmburns',
    'frostedglass',
    'glitch',
    'glitchblocks',
    'glitchchroma',
    'glitchlines',
    'glitchvideo',
    'glitter',
    'grain',
    'grainnoise',
    'hearts',
    'ice',
    'levelsramp',
    'neonframe',
    'neonframefrance',
    'neonframerainbow',
    'neonframesmoke',
    'neonframestraight',
    'noise',
    'oldvideo',
    'papergrain',
    'plastic',
    'radialblurchroma',
    'rotatecontinuous',
    'shadowflat',
    'shadowsoft',
    'sliderepeat',
    'slideshow',
    'slideshowblackwhite',
    'smokebottom',
    'smokeframe',
    'snow',
    'snow2',
    'stripesshadows',
    'super8frame',
    'super8frame2',
    'super8frame3',
    'super8frame4',
    'swirlout',
    'vhs',
    'vhs2',
    'vhssoft',
    'vhsstrong',
  ]

  foreach test_name : tests_effects
    func_name = 'effect_@0@'.format(test_name)
    ref_base = meson.current_source_dir() / 'refs/@0@'.format(func_name)
    test(
      test_name,
      ngl_test,
      args: [files('effects.py'), func_name, ref_base],
      env: env,
      suite: [backend, 'effects'],
    )
  endforeach

  # anim tests (pure math, no rendering — uses test_floats comparator with .ref files)
  tests_anim = [
    'forward_api',
    'derivative_api',
    'resolution_api',
    'forward_float',
    'forward_vec2',
    'forward_vec3',
    'forward_vec4',
    'forward_quat',
  ]

  foreach test_name : tests_anim
    func_name = 'anim_@0@'.format(test_name)
    ref_filepath = meson.current_source_dir() / 'refs/@0@.ref'.format(func_name)
    test(
      test_name,
      ngl_test,
      args: [files('anim.py'), func_name, ref_filepath],
      env: env,
      suite: [backend, 'anim'],
    )
  endforeach

  # transform tests (DrawRect + test_png)
  tests_transform = [
    'matrix',
    'translate',
    'translate_animated',
    'scale',
    'scale_animated',
    'scale_anchor',
    'scale_anchor_animated',
    'skew',
    'skew_animated',
    'rotate',
    'rotate_anchor',
    'rotate_quat',
    'rotate_quat_anchor',
    'rotate_quat_animated',
    'smoothpath',
    'shared_anim',
  ]

  foreach test_name : tests_transform
    func_name = 'transform_@0@'.format(test_name)
    ref_base = meson.current_source_dir() / 'refs/@0@'.format(func_name)
    test(
      test_name,
      ngl_test,
      args: [files('transform.py'), func_name, ref_base],
      env: env,
      suite: [backend, 'transform'],
    )
  endforeach

  # color tests (DrawRect + CustomFill + test_png)
  tests_color = [
    'static_srgb',
    'static_hsl',
    'static_hsv',
    'negative_values_srgb',
  ]

  foreach test_name : tests_color
    func_name = 'color_@0@'.format(test_name)
    ref_base = meson.current_source_dir() / 'refs/@0@'.format(func_name)
    test(
      test_name,
      ngl_test,
      args: [files('color.py'), func_name, ref_base],
      env: env,
      suite: [backend, 'color'],
    )
  endforeach

  # noise tests (DrawRect + NoiseFill + test_png)
  tests_noise = [
    'blocky',
    'perlin',
  ]

  foreach test_name : tests_noise
    func_name = 'noise_@0@'.format(test_name)
    ref_base = meson.current_source_dir() / 'refs/@0@'.format(func_name)
    test(
      test_name,
      ngl_test,
      args: [files('noise.py'), func_name, ref_base],
      env: env,
      suite: [backend, 'noise'],
    )
  endforeach

  # compositing tests (DrawRect + CustomFill + test_png)
  tests_compositing = [
    'src_over',
    'dst_over',
    'src_out',
    'dst_out',
    'src_in',
    'dst_in',
    'src_atop',
    'dst_atop',
    'xor',
  ]

  foreach test_name : tests_compositing
    func_name = 'compositing_@0@'.format(test_name)
    ref_base = meson.current_source_dir() / 'refs/@0@'.format(func_name)
    test(
      test_name,
      ngl_test,
      args: [files('compositing.py'), func_name, ref_base],
      env: env,
      suite: [backend, 'compositing'],
    )
  endforeach

  # data tests (DrawRect + EvalVec3/NoiseFloat/Time + test_png)
  tests_data = [
    'noise_time',
    'noise_wiggle',
  ]

  foreach test_name : tests_data
    func_name = 'data_@0@'.format(test_name)
    ref_base = meson.current_source_dir() / 'refs/@0@'.format(func_name)
    test(
      test_name,
      ngl_test,
      args: [files('data.py'), func_name, ref_base],
      env: env,
      suite: [backend, 'data'],
    )
  endforeach

  # userlive tests (DrawRect + test_png)
  tests_userlive = [
    'switch',
    'select',
  ]

  foreach test_name : tests_userlive
    func_name = 'userlive_@0@'.format(test_name)
    ref_base = meson.current_source_dir() / 'refs/@0@'.format(func_name)
    test(
      test_name,
      ngl_test,
      args: [files('userlive.py'), func_name, ref_base],
      env: env,
      suite: [backend, 'userlive'],
    )
  endforeach

  # blur tests (DrawRect + GaussianBlur/FastGaussianBlur/HexagonalBlur + test_png)
  tests_blur = [
    'gaussian',
    'fast_gaussian',
    'hexagonal',
    'hexagonal_with_map',
  ]

  foreach test_name : tests_blur
    func_name = 'blur_@0@'.format(test_name)
    ref_base = meson.current_source_dir() / 'refs/@0@'.format(func_name)
    test(
      test_name,
      ngl_test,
      args: [files('blur.py'), func_name, ref_base],
      env: env,
      suite: [backend, 'blur'],
    )
  endforeach

  # media tests using test_png comparator (PNG ref files)
  tests_media_png = [
    'flat_remap',
    'queue',
    'timeranges_rtt',
  ]

  foreach test_name : tests_media_png
    func_name = 'media_@0@'.format(test_name)
    ref_base = meson.current_source_dir() / 'refs/@0@'.format(func_name)
    test(
      test_name,
      ngl_test,
      args: [files('media.py'), func_name, ref_base],
      env: env,
      suite: [backend, 'media'],
    )
  endforeach

  # media tests using text-based comparators (.ref files)
  tests_media_ref = [
    'phases_display',
    'clamp',
  ]

  foreach test_name : tests_media_ref
    func_name = 'media_@0@'.format(test_name)
    ref_filepath = meson.current_source_dir() / 'refs/@0@.ref'.format(func_name)
    test(
      test_name,
      ngl_test,
      args: [files('media.py'), func_name, ref_filepath],
      env: env,
      suite: [backend, 'media'],
    )
  endforeach

endforeach
