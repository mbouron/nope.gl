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
add_project_arguments(cc.get_supported_arguments(['-Werror=missing-prototypes', '-Werror=vla']), language: 'c')

libnae = library('nae', 'src/nae.c', include_directories: inc, dependencies: math_dep,
                 c_args: build_args, install: true, version: meson.project_version(),
                 gnu_symbol_visibility: 'hidden', install_rpath: install_rpath)
nae_dep = declare_dependency(link_with: libnae, include_directories: inc, compile_args: api_args)
pkg.generate(libnae, name: 'libnae', description: 'nope audio engine', extra_cflags: api_args)
install_headers('src/include/nae/nae.h', subdir: 'nae')

nopemd_dep = dependency('libnopemd', required: get_option('nopemd'))
if nopemd_dep.found()
  libnae_nopemd = library('nae-nopemd', 'src/source_nopemd.c',
    dependencies: [nae_dep, nopemd_dep, math_dep], c_args: api_args + ['-DNAE_NOPEMD_BUILD'],
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

if get_option('tests')
  # Tests use assert for their checks, including in release configurations.
  test_args = cc.get_supported_arguments(['-UNDEBUG'])
  test_core = executable('test_nae', 'tests/test_nae.c', dependencies: [nae_dep, math_dep], c_args: test_args)
  test('nae core', test_core)
  if host_machine.system() != 'windows' and cc.has_header('pthread.h')
    test_threads = executable('test_threads', 'tests/test_threads.c',
      dependencies: [nae_dep, dependency('threads')], c_args: test_args)
    test('nae concurrent edits', test_threads, timeout: 60)
  endif
  if nopemd_dep.found()
    python = import('python').find_installation()
    fixture = custom_target('audio fixture', output: ['fixture.wav', 'fixture_44100.wav'],
      command: [python, files('tests/make_audio.py'), '@OUTPUT0@', '@OUTPUT1@'])
    test_media = executable('test_media', 'tests/test_media.c',
      dependencies: [nae_nopemd_dep, math_dep], c_args: test_args)
    test('nae nope.media', test_media, args: [fixture], timeout: 30)
    if host_machine.system() != 'windows' and cc.has_header('pthread.h')
      test_media_threads = executable('test_media_threads', 'tests/test_media_threads.c',
        dependencies: [nae_nopemd_dep, math_dep, dependency('threads')], c_args: test_args)
      test('nae concurrent PCM', test_media_threads, args: [fixture[0]], timeout: 30)
    endif
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
