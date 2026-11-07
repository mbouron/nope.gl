project('libnae', 'c', version: files('VERSION'), license: 'Apache-2.0',
        meson_version: '>=1.4.0', default_options: ['c_std=c23,c2x', 'warning_level=2'])

if get_option('default_library') == 'both'
  error('Use separate shared and static build directories (Windows API export definitions differ).')
endif

cc = meson.get_compiler('c')
inc = include_directories('src/include')
math_dep = cc.find_library('m', required: false)
pkg = import('pkgconfig')
install_rpath = get_option('rpath') ? get_option('prefix') / get_option('libdir') : ''
api_args = get_option('default_library') == 'static' ? ['-DNAE_STATIC'] : []
build_args = api_args + ['-DNAE_BUILD']
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
  '-Wcast-function-type',
  '-Wcast-function-type-strict',
])
if cc.get_id() == 'clang-cl'
  project_args += ['-Wno-format-signedness', '-Wno-sign-conversion']
endif
add_project_arguments(project_args, language: 'c')

libnae = library('nae', 'src/nae.c', 'src/transport.c', include_directories: inc, dependencies: math_dep,
                 c_args: build_args, install: true, version: meson.project_version(),
                 gnu_symbol_visibility: 'hidden', install_rpath: install_rpath)
nae_dep = declare_dependency(link_with: libnae, include_directories: inc, compile_args: api_args)
pkg.generate(libnae, name: 'libnae', description: 'nope audio engine', extra_cflags: api_args)
install_headers('src/include/nae/nae.h', subdir: 'nae')

nopemd_dep = dependency('libnopemd', required: get_option('nopemd'))
if nopemd_dep.found()
  libnae_nopemd = library('nae-nopemd', 'src/source_nopemd.c',
    dependencies: [nae_dep, nopemd_dep, math_dep, dependency('threads')], c_args: api_args + ['-DNAE_NOPEMD_BUILD'],
    install: true, version: meson.project_version(), gnu_symbol_visibility: 'hidden',
    install_rpath: install_rpath)
  nae_nopemd_dep = declare_dependency(link_with: libnae_nopemd, dependencies: nae_dep)
  pkg.generate(libnae_nopemd, name: 'libnae-nopemd', requires: 'libnae',
               description: 'nope.media source adapter for nae', extra_cflags: api_args)
  install_headers('src/include/nae/nopemd.h', subdir: 'nae')
endif

# miniaudio is a normal external dependency. Never compile its implementation
# inside nae, or define configuration macros different from the installed .pc.
miniaudio_dep = dependency('miniaudio', version: '=0.11.26', required: get_option('miniaudio'), static: true)
if miniaudio_dep.found()
  sink_platform_deps = []
  sink_platform_flags = []
  if host_machine.system() == 'darwin' and cc.compiles('''
    #include <TargetConditionals.h>
    #if !TARGET_OS_IPHONE
    #error not an iOS target
    #endif
    int main(void) { return 0; }
  ''', name: 'iOS audio session')
    # miniaudio's iOS implementation contains Objective-C AVAudioSession calls.
    sink_platform_deps += dependency('appleframeworks', modules: ['Foundation', 'AVFoundation'])
    sink_platform_flags += ['-framework Foundation', '-framework AVFoundation']
  endif
  libnae_miniaudio = library('nae-miniaudio', 'src/sink_miniaudio.c',
    dependencies: [nae_dep, miniaudio_dep, math_dep] + sink_platform_deps,
    c_args: api_args + ['-DNAE_MINIAUDIO_BUILD'],
    install: true, version: meson.project_version(), gnu_symbol_visibility: 'hidden',
    install_rpath: install_rpath)
  nae_miniaudio_dep = declare_dependency(link_with: libnae_miniaudio, dependencies: nae_dep)
  # A shared adapter includes the static miniaudio archive; consumers (including
  # Android Prefab) need neither its headers nor its .pc file. The static adapter
  # does require miniaudio at final link time. Avoid automatic private metadata
  # from pkg.generate(target), which would expose miniaudio in both cases.
  pc = configuration_data()
  pc.set('PREFIX', get_option('prefix'))
  pc.set('LIBDIR', '${prefix}' / get_option('libdir'))
  pc.set('INCLUDEDIR', '${prefix}' / get_option('includedir'))
  pc.set('VERSION', meson.project_version())
  pc.set('PRIVATE_REQUIRES', get_option('default_library') == 'static' ? 'miniaudio = 0.11.26' : '')
  pc.set('PRIVATE_LIBS', ' '.join((math_dep.found() ? ['-lm'] : []) + sink_platform_flags))
  pc.set('API_FLAGS', ' '.join(api_args))
  configure_file(input: 'libnae-miniaudio.pc.in', output: 'libnae-miniaudio.pc',
                 configuration: pc, install_dir: get_option('libdir') / 'pkgconfig')
  install_headers('src/include/nae/miniaudio.h', subdir: 'nae')
endif

if nopemd_dep.found() and miniaudio_dep.found()
  libnae_player = library('nae-player', 'src/player.c',
    dependencies: [nae_nopemd_dep, nae_miniaudio_dep, math_dep, dependency('threads')],
    c_args: api_args + ['-DNAE_PLAYER_BUILD'],
    install: true, version: meson.project_version(), gnu_symbol_visibility: 'hidden',
    install_rpath: install_rpath)
  nae_player_dep = declare_dependency(link_with: libnae_player,
    dependencies: [nae_nopemd_dep, nae_miniaudio_dep])
  pkg.generate(libnae_player, name: 'libnae-player', requires: ['libnae', 'libnae-nopemd', 'libnae-miniaudio'],
               description: 'Player of tracks of media files for nae', extra_cflags: api_args)
  install_headers('src/include/nae/player.h', subdir: 'nae')
endif

if get_option('tests')
  # Tests use assert for their checks, including in release configurations.
  test_args = cc.get_supported_arguments(['-UNDEBUG'])
  test_spsc = executable('test_spsc', 'tests/test_spsc.c', c_args: test_args)
  test('nae SPSC', test_spsc)
  test_core = executable('test_nae', 'tests/test_nae.c', dependencies: [nae_dep, math_dep], c_args: test_args)
  test('nae core', test_core)
  test_transport = executable('test_transport', 'tests/test_transport.c', dependencies: [nae_dep, math_dep], c_args: test_args)
  test('nae transport', test_transport)
  if host_machine.system() != 'windows' and cc.has_header('pthread.h')
    test_threads = executable('test_threads', 'tests/test_threads.c',
      dependencies: [nae_dep, dependency('threads')], c_args: test_args)
    test('nae concurrent edits', test_threads, timeout: 60)
  endif
  if nopemd_dep.found()
    python = import('python').find_installation()
    fixture = custom_target('audio fixture',
      output: ['fixture.wav', 'fixture_44100.wav', 'fixture_index.wav', 'fixture_mono.wav'],
      command: [python, files('tests/make_audio.py'), '@OUTPUT0@', '@OUTPUT1@', '@OUTPUT2@', '@OUTPUT3@'])
    test_media = executable('test_media', 'tests/test_media.c',
      dependencies: [nae_nopemd_dep, math_dep], c_args: test_args)
    # An AAC media of nope.media's tests (8 kHz mono, its first frame 128 ms)
    aac_fixture = files('../libnopemd/tests/h264_long_audio.mp4')
    test('nae nope.media', test_media, args: [fixture, aac_fixture], timeout: 30)
    if host_machine.system() != 'windows' and cc.has_header('pthread.h')
      test_media_threads = executable('test_media_threads', 'tests/test_media_threads.c',
        dependencies: [nae_nopemd_dep, math_dep, dependency('threads')], c_args: test_args)
      test('nae concurrent PCM', test_media_threads, args: [fixture[2]], timeout: 120)
    endif
  endif
  if nopemd_dep.found() and miniaudio_dep.found() and host_machine.system() != 'windows' and cc.has_header('pthread.h')
    player_test_args = test_args + (cc.has_header('valgrind/valgrind.h') ? ['-DHAVE_VALGRIND_H=1'] : [])
    test_player = executable('test_player', 'tests/test_player.c',
      dependencies: [nae_player_dep, math_dep, dependency('threads')], c_args: player_test_args)
    test('nae player', test_player, args: [fixture[0], fixture[2], meson.current_build_dir()], timeout: 120)
  endif
  if miniaudio_dep.found()
    test_sink = executable('test_sink', 'tests/test_sink.c', dependencies: nae_miniaudio_dep, c_args: test_args)
    test('nae null sink', test_sink, timeout: 10)
  endif
endif

if get_option('examples') and nopemd_dep.found() and miniaudio_dep.found()
  executable('nae-play', 'examples/play.c', dependencies: [nae_nopemd_dep, nae_miniaudio_dep],
             install: true, install_rpath: install_rpath)
endif
