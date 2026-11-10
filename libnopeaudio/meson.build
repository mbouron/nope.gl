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
  'src/effect/effect_lowpass.c',
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


#
# Tests
#

if get_option('tests')
  # Tests use assert for their checks, including in release configurations
  test_args = cc.get_supported_arguments(['-UNDEBUG'])

  # The backend tests build a backend of their own with mocks: they link the
  # objects of the core and of the sink rather than the library and its backend
  sink_objects = libnopeaudio.extract_objects(core_src, 'src/sink/sink.c')
  cpp_test_args = []

  # The SPSC tests build the queue with the allocator of the library
  memory_objects = libnopeaudio.extract_objects('src/utils/memory.c', 'src/utils/spsc_queue.c')

  test_progs = {
    'test_spsc': {'src': files('tests/test_spsc.c'), 'objects': memory_objects},
    'test_engine': {'src': files('tests/test_engine.c')},
    'test_lifecycle': {'src': files('tests/test_lifecycle.c')},
    'test_transport': {'src': files('tests/test_transport.c')},
    'test_node': {'src': files('tests/test_node.c')},
    'test_sink_null': {'src': files('tests/test_sink_null.c', 'src/utils/time.c')},
  }
  tests = {
    'SPSC queue': {'exe': 'test_spsc'},
    'Engine':     {'exe': 'test_engine'},
    'Transport':  {'exe': 'test_transport'},
    'Node lifecycle': {'exe': 'test_node'},
    'Null sink lifecycle': {'exe': 'test_sink_null'},
  }

  # The Oboe sink test runs on the host, with a mock of Oboe
  if host_system not in ['android', 'windows'] and add_languages('cpp', required: false, native: false)
    cpp_test_args = meson.get_compiler('cpp').get_supported_arguments(['-Wno-unused-parameter'])
    test_progs += {
      'test_sink_oboe': {
        'src': files('tests/test_sink_oboe.cpp', 'src/sink/sink_oboe.cpp', 'src/sink/sink_null.c'),
        'c_args': ['-DNAU_SINK_NULL_THREAD_ONLY'],
        'objects': sink_objects,
        'include_directories': include_directories('tests/oboe'),
        'override_options': ['cpp_std=c++17'],
      },
    }
    tests += {'Oboe sink failures': {'exe': 'test_sink_oboe', 'timeout': 10}}
  endif

  has_pthread = host_system != 'windows' and cc.has_header('pthread.h')
  if has_pthread
    test_progs += {
      'test_spsc_threads': {'src': files('tests/test_spsc_threads.c'), 'objects': memory_objects},
      'test_threads': {'src': files('tests/test_threads.c')},
    }
    tests += {
      'SPSC queue threads': {'exe': 'test_spsc_threads', 'timeout': 60},
      'Concurrent edits':   {'exe': 'test_threads', 'timeout': 60},
    }
  endif

  if nmd_dep.found()
    test_progs += {
      'test_export_wav': {'src': files('tests/test_export_wav.c', 'src/export/export.c')},
      'test_export': {'src': files('tests/test_export.c', 'src/export/export.c')},
    }
    tests += {
      'WAV export': {'exe': 'test_export_wav', 'args': [meson.current_build_dir() / 'test.wav', meson.current_build_dir()]},
      'Exporter codecs': {'exe': 'test_export', 'args': [meson.current_build_dir() / 'test-exporter.audio']},
    }
    python = import('python').find_installation()
    fixture = custom_target(
      'audio fixture',
      output: ['fixture.wav', 'fixture_44100.wav', 'fixture_index.wav', 'fixture_mono.wav', 'fixture_quiet.wav'],
      command: [python, files('tests/make_audio.py'), '@OUTPUT0@', '@OUTPUT1@', '@OUTPUT2@', '@OUTPUT3@', '@OUTPUT4@'],
    )
    # An AAC media of nope.media's tests (8 kHz mono, its first frame 128 ms)
    aac_fixture = files('../libnopemedia/tests/h264_long_audio.mp4')
    # Six quiet seconds normalization lifts, by a gain varying with them: a
    # 440 Hz tone, its amplitude going between 0.1 and 0.3 over 3.3 s, in AAC
    # (16 kHz mono)
    quiet_fixture = files('tests/quiet.m4a')
    # Two 10 ms stereo PCM packets (0.25, -0.25), separated by 500 ms:
    # ffmpeg -f lavfi -i 'aevalsrc=0.25|-0.25:s=48000:d=0.02' \
    #   -af 'asetnsamples=n=480:p=0,asetpts=PTS+gte(N\,480)*24000' \
    #   -c:a pcm_s16le -fflags +bitexact gap.mka
    gap_fixture = files('tests/gap.mka')

    test_progs += {'test_media': {'src': files('tests/test_media.c', 'src/utils/time.c')}}
    tests += {'nope.media source': {'exe': 'test_media', 'args': [fixture, aac_fixture, quiet_fixture, gap_fixture]}}
    if has_pthread
      test_progs += {'test_media_threads': {'src': files('tests/test_media_threads.c', 'src/utils/time.c')}}
      tests += {'Concurrent PCM reads': {'exe': 'test_media_threads', 'args': [fixture[2]], 'timeout': 120}}
    endif
  endif

  # The tests play on the null device of the sinks. Oboe builds are for
  # Android: their tests do not run on the host.
  if sink != 'oboe'
    if has_player and has_pthread
      test_progs += {
        'test_player': {
          'src': files('tests/test_player.c', 'src/utils/time.c'),
          'c_args': cc.has_header('valgrind/valgrind.h') ? ['-DHAVE_VALGRIND_H=1'] : [],
        },
      }
      tests += {'Player': {'exe': 'test_player', 'args': [fixture[0], fixture[2], meson.current_build_dir(), gap_fixture], 'timeout': 120}}
    endif
    test_progs += {'test_sink': {'src': files('tests/test_sink.c', 'src/utils/time.c')}}
    tests += {'Null device sink': {'exe': 'test_sink', 'timeout': 10}}
  endif
  if sink == 'null'
    tests += {'Null backend sink': {'exe': 'test_sink', 'args': ['--device'], 'timeout': 10}}
    tests += {'Null backend lifecycle': {'exe': 'test_sink_null', 'args': ['default']}}
  elif sink == 'sdl3'
    test_progs += {'test_sink_sdl3_events': {
      'src': files('tests/test_sink_sdl3_events.c', 'src/sink/sink_null.c'),
      'objects': sink_objects,
      'c_args': ['-DNAU_SINK_NULL_THREAD_ONLY'],
    }}
    tests += {
      'SDL3 dummy sink':  {'exe': 'test_sink', 'args': ['--device'], 'env': ['SDL_AUDIO_DRIVER=dummy'], 'timeout': 10},
      'SDL3 sink events': {'exe': 'test_sink_sdl3_events', 'env': ['SDL_AUDIO_DRIVER=dummy'], 'timeout': 10},
    }
    if host_system != 'windows'
      test_progs += {'test_sink_sdl3_runtime': {'src': files('tests/test_sink_sdl3_runtime.c')}}
      tests += {'SDL3 sink runtime': {'exe': 'test_sink_sdl3_runtime', 'env': ['SDL_AUDIO_DRIVER=dummy'], 'timeout': 10}}
    endif
  endif

  # The player half of the contract test decodes and exports a fixture
  if nmd_dep.found()
    tests += {'Lifecycle': {'exe': 'test_lifecycle', 'args': [fixture[0], meson.current_build_dir() / 'lifecycle.wav']}}
  else
    tests += {'Lifecycle': {'exe': 'test_lifecycle'}}
  endif

  executables = {}
  foreach exe_name, exe_data : test_progs
    # A test on the objects of the library builds its own copy of the API
    standalone = 'objects' in exe_data
    exe_args = test_args + exe_data.get('c_args', []) + (standalone ? ['-DUSE_NOPE_AUDIO_STATIC_LIB'] : pkg_extra_cflags)
    exe = executable(
      exe_name,
      exe_data.get('src'),
      objects: exe_data.get('objects', []),
      dependencies: lib_deps,
      include_directories: [inc_dir, exe_data.get('include_directories', [])],
      implicit_include_directories: false,
      link_with: standalone ? [] : libnopeaudio,
      install: false,
      c_args: exe_args,
      cpp_args: exe_args + cpp_test_args,
      override_options: exe_data.get('override_options', []),
    )
    executables += {exe_name: exe}
  endforeach

  foreach test_name, test_data : tests
    test(
      test_name,
      executables.get(test_data.get('exe')),
      args: test_data.get('args', []),
      env: test_data.get('env', []),
      timeout: test_data.get('timeout', 30),
    )
  endforeach
endif
