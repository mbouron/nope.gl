#
# Copyright 2026 Matthieu Bouron <matthieu.bouron@gmail.com>
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
  'libnopeaudio',
  'c',
  default_options: ['c_std=c23,c2x', 'warning_level=2'],
  license: 'Apache-2.0',
  meson_version: '>= 1.4.0',
  version: files('VERSION'),
)

conf_data = configuration_data()
version_array = meson.project_version().split('.')
conf_data.set('version_major', version_array[0])
conf_data.set('version_minor', version_array[1])
conf_data.set('version_micro', version_array[2])

host_system = host_machine.system()
cc = meson.get_compiler('c')

install_rpath = get_option('rpath') ? get_option('prefix') / get_option('libdir') : ''

if get_option('default_library') == 'both'
  error('Use separate shared and static build directories (Windows API export definitions differ).')
endif

if host_system == 'windows'
  if cc.get_id() == 'msvc'
    error('MSVC cl.exe is not supported, use clang-cl instead')
  elif cc.get_id() == 'clang-cl'
    add_project_arguments(['-D_CRT_SECURE_NO_WARNINGS'], language: 'c')
  endif
endif

# Keep __FILE__ relative to the source directory in out-of-tree builds.
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
  '-Wno-unused-parameter',
  '-fmacro-prefix-map=@0@/='.format(trim_prefix),
  '-fno-math-errno',
  '-Wcast-function-type',
  '-Wcast-function-type-strict',
])
if cc.get_id() == 'clang-cl'
  project_args += ['-Wno-format-signedness', '-Wno-sign-conversion']
endif
add_project_arguments(project_args, language: 'c')

# The targets do not include the source directory implicitly: its VERSION file
# would shadow the <version> header of C++ on case-insensitive file systems
inc_dir = include_directories(
  'src',
  'src/include',
)


#
# Dependencies
#

# Threads are used by the null device and media decoders.
lib_deps = [
  cc.find_library('m', required: false),
  dependency('threads'),
]

nmd_dep = dependency('libnopemedia', required: get_option('nopemedia'), include_type: 'system')
if nmd_dep.found()
  lib_deps += nmd_dep
  export_deps = [
    dependency('libavformat', version: '>= 60.16.100', include_type: 'system'),
    dependency('libavcodec', version: '>= 60.31.102', include_type: 'system'),
    dependency('libavutil', version: '>= 58.29.100', include_type: 'system'),
    dependency('libswresample', version: '>= 4.12.100', include_type: 'system'),
  ]
  lib_deps += export_deps
endif

sink = get_option('sink')

oboe_dep = dependency('', required: false)
if sink in ['auto', 'oboe']
  oboe_dep = dependency('oboe', static: true, include_type: 'system', required: sink == 'oboe')
endif

sdl3_dep = dependency('', required: false)
if sink == 'sdl3' or (sink == 'auto' and not oboe_dep.found())
  sdl3_dep = dependency('sdl3', version: '>= 3.2.0', required: sink == 'sdl3', include_type: 'system')
endif

if sink == 'auto'
  if oboe_dep.found()
    sink = 'oboe'
  elif sdl3_dep.found()
    sink = 'sdl3'
  else
    sink = 'null'
  endif
endif

if sink == 'oboe'
  lib_deps += oboe_dep
elif sink == 'sdl3'
  lib_deps += sdl3_dep
endif

has_player = nmd_dep.found()

conf_data.set10('NAU_DEFAULT_SINK_NULL', sink == 'null')

conf_data.set10('have_nopemedia', nmd_dep.found())


#
# Library
#

core_src = files(
  'src/engine.c',
  'src/effect/effect.c',
  'src/effect/effect_passthrough.c',
  'src/node/node_fader.c',
  'src/node/node_gain.c',
  'src/node/node_mix.c',
  'src/ramp.c',
  'src/transport/transport.c',
  'src/transport/transport_clock.c',
  'src/utils/memory.c',
  'src/utils/semaphore.c',
  'src/utils/spsc_queue.c',
  'src/utils/thread.c',
  'src/utils/time.c',
)

lib_src = core_src
if nmd_dep.found()
  lib_src += files('src/source/source_nopemedia.c')
endif
sink_src = files('src/sink/sink.c', 'src/sink/sink_null.c')
lib_src += sink_src
if sink == 'oboe'
  lib_src += files('src/sink/sink_oboe.cpp')
elif sink == 'sdl3'
  lib_src += files('src/sink/sink_sdl3.c')
endif
if has_player
  lib_src += files('src/player.c', 'src/export/export.c')
endif

lib_c_args = []
if get_option('default_library') == 'shared'
  lib_c_args += '-DBUILD_NOPE_AUDIO_SHARED_LIB'
else
  lib_c_args += '-DUSE_NOPE_AUDIO_STATIC_LIB'
endif

lib_cpp_args = []
lib_override_options = []
if sink == 'oboe'
  add_languages('cpp', native: false)
  cpp = meson.get_compiler('cpp')
  lib_cpp_args += lib_c_args + cpp.get_supported_arguments(['-Wshadow', '-Wconversion', '-Wno-unused-parameter'])
  lib_override_options += 'cpp_std=c++17'
endif

subdir('src')

libnopeaudio = library(
  'nopeaudio',
  lib_src,
  dependencies: lib_deps,
  install: true,
  install_rpath: install_rpath,
  version: meson.project_version(),
  c_args: lib_c_args,
  cpp_args: lib_cpp_args,
  override_options: lib_override_options,
  gnu_symbol_visibility: 'hidden',
  include_directories: inc_dir,
  implicit_include_directories: false,
)
subdir('src/include/nopeaudio')

pkg = import('pkgconfig')
pkg_extra_cflags = []
if get_option('default_library') == 'static'
  pkg_extra_cflags += '-DUSE_NOPE_AUDIO_STATIC_LIB'
endif

pkg.generate(
  libnopeaudio,
  name: 'libnopeaudio',  # not specifying the name would fallback on "nopeaudio.pc" instead of "libnopeaudio.pc"
  description: 'nope.audio library',
  extra_cflags: pkg_extra_cflags,
)
