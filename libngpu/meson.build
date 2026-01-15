#
# Copyright 2023-2026 Matthieu Bouron <matthieu.bouron@gmail.com>
# Copyright 2023 Nope Forge
# Copyright 2020-2022 GoPro Inc.
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

project(
  'libngpu',
  'c',
   default_options: ['c_std=c17'],
   license: 'Apache',
   meson_version: '>= 0.57.0',
   version: files('VERSION'),
)

conf_data = configuration_data()
version_array = meson.project_version().split('.')
conf_data.set('version_major', version_array[0])
conf_data.set('version_minor', version_array[1])
conf_data.set('version_micro', version_array[2])

host_system = host_machine.system()
cpu_family = host_machine.cpu_family()
cc = meson.get_compiler('c')

# meson currently (0.56.0) doesn't distinguish between macOS and iOS (the
# "darwin" identifier is shared), so we need to test explicitly.
# We are also checking for IPHONE and not IOS but since the latter is actually
# more specific (doesn't include tvOS and watchOS) we may want to consider
# switching to it at some point.
# See https://github.com/mesonbuild/meson/issues/7944 for more information
iphone_check = '''#include <TargetConditionals.h>
#if !TARGET_OS_IPHONE
#error not iphone
#endif'''
if host_system == 'darwin' and cc.compiles(iphone_check, name: 'iPhone target')
  host_system = 'iphone'
endif

conf_data.set10('HAVE_USELOCALE', cc.has_function('uselocale', prefix: '''
#define _POSIX_C_SOURCE 200809L
#include <locale.h>
#ifdef __APPLE__
# include <xlocale.h>
#endif
'''))

# Builtins checks
builtins = [
  '__builtin_add_overflow',
  '__builtin_sub_overflow',
  '__builtin_mul_overflow',
]
has_builtin_overflow = true
foreach builtin : builtins
  if not cc.has_function(builtin)
    warning('Builtin @0@ is not available, important security checks will be missing'.format(builtin))
    has_builtin_overflow = false
  endif
endforeach
conf_data.set10('HAVE_BUILTIN_OVERFLOW', has_builtin_overflow)

mandatory_builtins = []
if cc.get_id() != 'msvc'
  mandatory_builtins += [
    '__builtin_clz',
  ]
endif
foreach builtin : mandatory_builtins
  if not cc.has_function(builtin)
    error('Builtin @0@ is not available'.format(builtin))
  endif
endforeach

install_rpath = get_option('rpath') ? get_option('prefix') / get_option('libdir') : ''

debug_opts = get_option('debug_opts')
conf_data.set10('TARGET_' + host_system.to_upper(), true)
conf_data.set10('ARCH_' + cpu_family.to_upper(), true)
conf_data.set10('DEBUG_GL', 'gl' in debug_opts)
conf_data.set10('DEBUG_VK', 'vk' in debug_opts)
conf_data.set10('DEBUG_MEM', 'mem' in debug_opts)
conf_data.set10('DEBUG_SCENE', 'scene' in debug_opts)
conf_data.set10('DEBUG_GPU_CAPTURE', 'gpu_capture' in debug_opts)

if host_system == 'windows'
  if cc.get_id() == 'msvc'
    conf_data.set10('TARGET_MSVC', true)
    msvc_args = [
      # Ignore several spurious warnings
      '/wd4018', # comparison between signed and unsigned numbers
      '/wd4146', # unary minus operator applied to unsigned type when its result is still unsigned (INT_MIN)
      '/wd4244', # conversion from 'type1' to 'type2', resulting in a possible loss of data
      '/wd4305', # value is converted to a smaller type in an initialization, resulting in a loss of information.
      # Enable some warnings to match GCC/Clang
      '/w14062', # enumerator 'identifier' in switch of enum 'enumeration' is not handled
      '/w14101', # 'identifier' : unreferenced local variable
      '/w14189', # 'identifier' : local variable is initialized but not referenced
    ]
    add_project_arguments(msvc_args, language: 'c')
  else
    conf_data.set10('TARGET_MINGW_W64', true)
    add_project_arguments(['-D__USE_MINGW_ANSI_STDIO=1'], language: 'c')
  endif
endif

# This trim prefix is used to make __FILE__ starts from the source dir with
# out-of-tree builds.
# Adjusted from https://github.com/mesonbuild/meson/issues/7485
trim_prefix = run_command([
  find_program('python3'), '-c', 'import sys,os;print(os.path.relpath(*sys.argv[1:3]))',
  meson.current_source_dir(),
  meson.project_build_root(),
], check: true).stdout().strip()

project_args = cc.get_supported_arguments([
  '-Wshadow',
  '-Werror=missing-prototypes',
  '-Werror=pointer-arith',
  '-Werror=vla',
  '-Wformat=2',
  '-Wformat-signedness',
  '-Wignored-qualifiers',
  '-Wimplicit-fallthrough',
  '-Wlogical-op',
  '-Wundef',
  '-Wconversion',
  '-fmacro-prefix-map=@0@/='.format(trim_prefix),
  '-fno-math-errno',
  '-Wcast-function-type',
  '-Wcast-function-type-strict',
])
add_project_arguments(project_args, language: 'c')

# Also apply to local generator targets such as gen_specs
# See https://github.com/mesonbuild/meson/issues/7940
add_project_arguments(project_args, language: 'c', native: true)

inc_dir = include_directories(
  'src',
  'src/include',
)

#
# Library main configuration
#

lib_version = '0.0.0'
lib_src = files(
  'src/bindgroup.c',
  'src/block.c',
  'src/block_desc.c',
  'src/buffer.c',
  'src/ctx.c',
  'src/format.c',
  'src/pipeline.c',
  'src/program.c',
  'src/pgcache.c',
  'src/pgcraft.c',
  'src/rendertarget.c',
  'src/texture.c',
  'src/type.c',
  'src/utils/bstr.c',
  'src/utils/crc32.c',
  'src/utils/darray.c',
  'src/utils/hmap.c',
  'src/utils/log.c',
  'src/utils/memory.c',
  'src/utils/refcount.c',
  'src/utils/string.c',
  'src/utils/thread.c',
  'src/utils/time.c',
)

hosts_cfg = {
  'linux': {
    'deps': ['x11'],
  },
  'darwin': {
    'frameworks': ['CoreVideo', 'CoreFoundation', 'AppKit', 'IOSurface'],
  },
  'android': {
    'libs': ['android'],
  },
  'iphone': {
    'frameworks': ['CoreVideo', 'Foundation', 'UIKit', 'IOSurface'],
  },
}

lib_deps = [
  cc.find_library('m', required: false),
  dependency('libnopemd', version: '>= 12.0.0'),
  dependency('threads'),
]

host_cfg = hosts_cfg.get(host_system, {})
foreach dep : host_cfg.get('deps', [])
  lib_deps += dependency(dep)
endforeach
foreach dep : host_cfg.get('libs', [])
  lib_deps += cc.find_library(dep)
endforeach
if 'frameworks' in host_cfg
  lib_deps += dependency('appleframeworks', modules: host_cfg.get('frameworks'))
endif
lib_src += host_cfg.get('src', [])


#
# VAAPI dependency
#

libva_version = '>= 1.1.0'
vaapi_enabled = false

opt_wayland = get_option('wayland')
dep_wayland_client = dependency('wayland-client', required: opt_wayland)
dep_wayland_egl    = dependency('wayland-egl',    required: opt_wayland)
if dep_wayland_client.found() and dep_wayland_egl.found()
  lib_deps += [dep_wayland_client, dep_wayland_egl]
  conf_data.set10('HAVE_WAYLAND', true)
endif

opt_vaapi_x11 = get_option('vaapi-x11')
dep_libva_x11 = dependency('libva-x11', version: libva_version, required: opt_vaapi_x11)
dep_libva_x11_drm = dependency('libva-drm', version: libva_version, required: opt_vaapi_x11)
if dep_libva_x11.found() and dep_libva_x11_drm.found()
  vaapi_enabled = true
  lib_deps += [dep_libva_x11, dep_libva_x11_drm]
  conf_data.set10('HAVE_VAAPI_X11', true)
endif

opt_vaapi_wayland = get_option('vaapi-wayland')
dep_libva_wayland = dependency('libva-wayland', version: libva_version, required: opt_vaapi_wayland)
dep_libva_wayland_drm = dependency('libva-drm', version: libva_version, required: opt_vaapi_wayland)
if dep_libva_wayland.found() and dep_libva_wayland_drm.found()
  vaapi_enabled = true
  lib_deps += [dep_libva_wayland, dep_libva_wayland_drm]
  conf_data.set10('HAVE_VAAPI_WAYLAND', true)
endif

if vaapi_enabled
  conf_data.set10('HAVE_VAAPI', true)
endif


#
# GPU Capture Options
#

if 'gpu_capture' in debug_opts
  renderdoc_dir = get_option('renderdoc_dir')
  renderdoc_include_dir = ''
  if host_system == 'windows'
    renderdoc_include_dir = renderdoc_dir
  else
    renderdoc_include_dir = renderdoc_dir / 'include'
  endif
  if cc.has_header(renderdoc_include_dir / 'renderdoc_app.h', required: true)
    lib_src += files('src/capture_renderdoc.c')
    lib_deps += declare_dependency(include_directories: renderdoc_include_dir)
    lib_deps += cc.find_library('dl', required: host_system == 'linux')
  endif
endif


#
# Graphic backend dependencies
#

gbackends_cfg = {
  'gl': {
    'src': files(
      'src/opengl/cmd_buffer_gl.c',
      'src/opengl/fence_gl.c',
      'src/opengl/glcontext.c',
      'src/opengl/glstate.c',
      'src/opengl/bindgroup_gl.c',
      'src/opengl/buffer_gl.c',
      'src/opengl/ctx_gl.c',
      'src/opengl/format_gl.c',
      'src/opengl/pipeline_gl.c',
      'src/opengl/program_gl.c',
      'src/opengl/program_gl_utils.c',
      'src/opengl/rendertarget_gl.c',
      'src/opengl/texture_gl.c',
    ),
    'cfg': 'BACKEND_GL',
    'hosts_cfg': {
      'linux': {
        'src': files('src/opengl/glcontext_egl.c'),
        'cfg': 'HAVE_GLPLATFORM_EGL',
        'deps': ['gl', 'egl'],
      },
      'darwin': {
        'src': files('src/opengl/glcontext_nsgl.m'),
        'cfg': 'HAVE_GLPLATFORM_NSGL',
        'frameworks': ['OpenGL'],
      },
      'windows': {
        'src': files('src/opengl/glcontext_wgl.c'),
        'cfg': 'HAVE_GLPLATFORM_WGL',
        'libs': ['OpenGL32', 'gdi32'],
      },
    },
  },
  'gles': {
    'src': files(
      'src/opengl/cmd_buffer_gl.c',
      'src/opengl/fence_gl.c',
      'src/opengl/format_gl.c',
      'src/opengl/glcontext.c',
      'src/opengl/glstate.c',
      'src/opengl/bindgroup_gl.c',
      'src/opengl/buffer_gl.c',
      'src/opengl/ctx_gl.c',
      'src/opengl/pipeline_gl.c',
      'src/opengl/program_gl.c',
      'src/opengl/program_gl_utils.c',
      'src/opengl/rendertarget_gl.c',
      'src/opengl/texture_gl.c',
    ),
    'cfg': 'BACKEND_GLES',
    'hosts_cfg': {
      'linux': {
        'src': files('src/opengl/glcontext_egl.c'),
        'cfg': 'HAVE_GLPLATFORM_EGL',
        'deps': ['gl', 'egl'],
      },
      'android': {
        'src': files('src/opengl/glcontext_egl.c'),
        'cfg': 'HAVE_GLPLATFORM_EGL',
        'libs': ['EGL'],
      },
      'iphone': {
        'src': files('src/opengl/glcontext_eagl.m'),
        'cfg': 'HAVE_GLPLATFORM_EAGL',
        'frameworks': ['OpenGLES'],
      },
      'windows': {
        'src': files('src/opengl/glcontext_wgl.c'),
        'cfg': 'HAVE_GLPLATFORM_WGL',
        'libs': ['OpenGL32', 'gdi32'],
      },
    },
  },
  'vk': {
    'src': files(
      'src/vulkan/cmd_buffer_vk.c',
      'src/vulkan/glslang_utils.c',
      'src/vulkan/bindgroup_vk.c',
      'src/vulkan/buffer_vk.c',
      'src/vulkan/ctx_vk.c',
      'src/vulkan/format_vk.c',
      'src/vulkan/pipeline_vk.c',
      'src/vulkan/program_vk.c',
      'src/vulkan/rendertarget_vk.c',
      'src/vulkan/texture_vk.c',
      'src/vulkan/vkcontext.c',
      'src/vulkan/vkutils.c',
      'src/vulkan/ycbcr_sampler_vk.c',
    ),
    'cfg': 'BACKEND_VK',
    'hosts_cfg': {
      'linux': {
        'deps': ['vulkan', 'glslang'],
      },
      'darwin': {
        'src': files(
          'src/vulkan/wsi_apple.m',
        ),
        'deps': ['glslang'],
        'libs': ['MoltenVK'],
        'frameworks': ['Metal', 'QuartzCore', 'CoreGraphics', 'IOKit'],
      },
      'android': {
        'deps': ['vulkan', 'glslang'],
      },
      'iphone': {
        'src': files(
          'src/vulkan/wsi_apple.m',
        ),
        'deps': ['glslang'],
        'libs': ['MoltenVK'],
        'frameworks': ['Metal', 'QuartzCore', 'CoreGraphics'],
      },
      'windows': {
        'deps': ['vulkan', 'glslang'],
      },
    },
  },
}

if host_system in ['darwin', 'iphone']
  add_languages('objc')
endif

# Additional search paths for find_library
extra_library_dirs = get_option('extra_library_dirs')
extra_include_dirs = get_option('extra_include_dirs')

foreach gbackend_name, gbackend_cfg : gbackends_cfg
  opt_gbackend = get_option('gbackend-' + gbackend_name)
  if opt_gbackend.disabled()
    continue
  endif
  hosts_backend_cfg = gbackend_cfg.get('hosts_cfg')
  if host_system not in hosts_backend_cfg
    continue
  endif
  host_backend_cfg = hosts_backend_cfg.get(host_system)
  gbackend_deps = []
  foreach dep : host_backend_cfg.get('deps', [])
    if dep == 'glslang'
      add_languages('cpp')
      cxx = meson.get_compiler('cpp')
      # The following list declares the glslang libraries that we need to check
      # and link against in the right order (to allow static linking). Some
      # libraries are marked as non-required because they might be present or
      # not depending on the build of glslang.
      glslang_libs = [
        {'name': 'glslang',                         'required': true},
        {'name': 'glslang-default-resource-limits', 'required': true},
        {'name': 'SPIRV-Tools-opt',                 'required': false},
        {'name': 'SPIRV-Tools',                     'required': false},
      ]
      foreach lib : glslang_libs
        libname = lib.get('name')
        # Only require the library if the backend is explicitly enabled by the
        # user. This makes meson bail out early if the required library can not
        # be found.
        required = lib.get('required') and opt_gbackend.enabled()
        dep = cxx.find_library(libname, required: required, dirs: extra_library_dirs)
        # Add the dependency if the library is marked as required or if it has
        # been found so the backend can be disabled later on.
        if lib.get('required') or dep.found()
          gbackend_deps += dep
        endif
      endforeach
      glslang_header = 'glslang/build_info.h'
      glslang_include_dirs = include_directories(extra_include_dirs)
      if cxx.has_header(glslang_header, include_directories: glslang_include_dirs)
        glslang_ver_array = []
        foreach comp : ['MAJOR', 'MINOR', 'PATCH']
          glslang_ver_array += cxx.get_define(
            'GLSLANG_VERSION_' + comp,
            prefix: '#include <' + glslang_header + '>',
            include_directories: glslang_include_dirs,
          )
        endforeach
        glslang_ver = '.'.join(glslang_ver_array)
        glslang_min_ver = '>= 15.3.0'
        if glslang_ver.version_compare(glslang_min_ver)
          gbackend_deps += declare_dependency(include_directories: glslang_include_dirs)
        else
          if opt_gbackend.enabled()
            error('detected glslang version @0@ is too old, must be @1@'.format(
                  glslang_ver, glslang_min_ver))
          else
            gbackend_deps += disabler()
          endif
        endif
      else
        if opt_gbackend.enabled()
          error('glslang header not found')
        else
          gbackend_deps += disabler()
        endif
      endif
    else
      gbackend_deps += dependency(dep, required: opt_gbackend)
    endif
  endforeach
  foreach lib : host_backend_cfg.get('libs', [])
    # We need to call find_library() twice because specifying the dirs
    # parameter inhibits system library detection.
    # See: https://github.com/mesonbuild/meson/issues/10120
    dep = cc.find_library(lib, required: false)
    if not dep.found()
      dep = cc.find_library(lib, required: opt_gbackend, dirs: extra_library_dirs)
      if dep.found()
        gbackend_deps += declare_dependency(include_directories: extra_include_dirs)
      endif
    endif
    gbackend_deps += dep
  endforeach
  if 'frameworks' in host_backend_cfg
    gbackend_deps += dependency('appleframeworks', modules: host_backend_cfg.get('frameworks'), required: opt_gbackend)
  endif

  all_dep_found = true
  foreach dep : gbackend_deps
    all_dep_found = all_dep_found and dep.found()
  endforeach
  message('backend: @0@ enabled: @1@'.format(gbackend_name, all_dep_found.to_string()))
  if all_dep_found

    lib_src += gbackend_cfg.get('src')
    lib_src += host_backend_cfg.get('src', [])
    lib_deps += gbackend_deps
    conf_data.set10(gbackend_cfg.get('cfg'), true)
    if 'cfg' in host_backend_cfg
      conf_data.set10(host_backend_cfg.get('cfg'), true)
    endif
  endif
endforeach


#
# Library
#

configure_file(output: 'config.h', configuration: conf_data)

lib_c_args = []
if get_option('default_library') == 'shared'
  lib_c_args += '-DBUILD_NGPU_SHARED_LIB'
else
  lib_c_args += '-DUSE_NGPU_STATIC_LIB'
endif

lib_objc_args = []
if host_system == 'darwin'
  lib_c_args += '-DGL_SILENCE_DEPRECATION'
  lib_objc_args += '-DGL_SILENCE_DEPRECATION'
elif host_system == 'iphone'
  lib_c_args += '-DGLES_SILENCE_DEPRECATION'
  lib_objc_args += '-DGLES_SILENCE_DEPRECATION'
endif


# Create ngpu version data for rc
ngpu_version_cdata = configuration_data()
ngpu_version_cdata.set('NGPU_COPYRIGHT', 'Copyright Nope Forge')
ngpu_version_cdata.set('NGPU_MAJOR_VERSION', version_array[0])
ngpu_version_cdata.set('NGPU_MINOR_VERSION', version_array[1])
ngpu_version_cdata.set('NGPU_MICRO_VERSION', version_array[2])
ngpu_version_cdata.set('NGPU_VERSION', meson.project_version())

# Add properties information with a rc file for the DLL under Windows
if cc.get_id() == 'msvc'
  ngpu_rc = configure_file(
    input: 'ngpu.rc.in',
    output: 'ngpu.rc',
    configuration: ngpu_version_cdata,
  )
  lib_src += import('windows').compile_resources(ngpu_rc)
endif

libngpu = library(
  'ngpu',
  lib_src,
  dependencies: lib_deps,
  install: true,
  install_rpath: install_rpath,
  version: meson.project_version(),
  c_args: lib_c_args,
  objc_args: lib_objc_args,
  gnu_symbol_visibility: 'hidden',
  include_directories: inc_dir,
)
subdir('src/include/ngpu')

pkg = import('pkgconfig')
pkg_extra_cflags = []

if get_option('default_library') == 'static'
  pkg_extra_cflags += '-DUSE_NGPU_STATIC_LIB'
endif

pkg.generate(
  libngpu,
  name: 'libngpu',  # not specifying the name would fallback on "ngpu.pc" instead of "libngpu.pc"
  description: 'NGPU API',
  extra_cflags: pkg_extra_cflags,
  variables: ['datarootdir=${prefix}/share'],
)


#
# OpenGL specifications
#
 
copyfile = find_program('scripts/copyfile.py')

gl_xml = custom_target(
  'gl.xml',
  command: [find_program('curl'), 'https://raw.githubusercontent.com/KhronosGroup/OpenGL-Registry/master/xml/gl.xml', '-o', '@OUTPUT@'],
  capture: false,
  output: 'gl.xml',
)
gl_generated_files = custom_target(
  'gl_generated_files',
  command: [find_program('scripts/gen-gl-wrappers.py'), '@INPUT@', '@OUTPUT@'],
  capture: false,
  input: gl_xml,
  output: ['glfunctions.h'],
)
run_target(
  'updateglwrappers',
  command: [
    copyfile,
    # FIXME: In 0.56.0, we get "WARNING: custom_target 'gl_generated_files' has
    # more than one output!  Using the first one.", so we need to do this
    # instead of just using gl_generated_files. It also requires the "depends:"
    # entry below.
    # See https://github.com/mesonbuild/meson/issues/7891 for more information
    gl_generated_files[0].full_path(),
    meson.current_source_dir() / 'src/opengl'
  ],
  depends: gl_generated_files,
)
